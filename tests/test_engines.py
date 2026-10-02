"""Realtime with real engines: whisper.cpp ASR, llama.cpp LLM, OuteTTS.
Opt-in: `make test-engines`.

Needs SPORED_ENGINES, SPORE_ASR_MODEL, SPORE_VOICE_LLM and SPORE_ASR_SAMPLE
(a 16 kHz mono WAV of JFK's "ask not" line, whisper.cpp's samples/jfk.wav);
the speech-output tests also need SPORE_TTS_MODEL and SPORE_VOCODER.
"""

import base64
import io
import json
import os
import re
import struct
import subprocess
import time
import urllib.request
import wave

import pytest

BIN = os.environ.get("SPORED_ENGINES")
ASR = os.environ.get("SPORE_ASR_MODEL")
LLM = os.environ.get("SPORE_VOICE_LLM")
SAMPLE = os.environ.get("SPORE_ASR_SAMPLE")
TTS = os.environ.get("SPORE_TTS_MODEL")
VOCODER = os.environ.get("SPORE_VOCODER")
needs_tts = pytest.mark.skipif(not (TTS and VOCODER), reason="TTS model or vocoder missing")

pytestmark = pytest.mark.skipif(
    not (BIN and ASR and LLM and SAMPLE and os.path.exists(SAMPLE)),
    reason="engine binary, models or sample missing",
)
wsclient = pytest.importorskip("websockets.sync.client")


def words(s):
    return re.sub(r"[^a-z ]", "", s.lower()).split()


def jfk_24k():
    """The sample as 24 kHz s16le, by linear interpolation from 16 kHz."""
    with wave.open(SAMPLE) as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
        x = struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))
    n = len(x) * 3 // 2
    y = []
    for i in range(n):
        t = i * 2 / 3
        k = int(t)
        a = x[min(k, len(x) - 1)]
        b = x[min(k + 1, len(x) - 1)]
        y.append(int(a + (b - a) * (t - k)))
    return struct.pack(f"<{n}h", *y)


GPU = ["--gpu"] if os.environ.get("SPORE_ENGINE_GPU") == "1" else []


def serve(*args):
    p = subprocess.Popen([BIN, "--port", "0", *GPU, *args], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline()
    assert "listening" in line, line
    return p, int(line.rsplit(":", 1)[1])


def tts_args():
    # 10 speaker words: ~2.5x shorter prompts, same intelligibility (measured)
    return ["--tts", TTS, "--vocoder", VOCODER, "--tts-speaker-words", "10"] if TTS and VOCODER else []


# Function scope: one server resident at a time. Two servers with the 1B
# TTS model do not fit an 8 GiB GPU, and ggml aborts on CUDA OOM.
@pytest.fixture
def port():
    p, port = serve("--model", LLM, "--ctx", "2048", "--asr", ASR, *tts_args())
    yield port
    p.terminate()
    assert p.wait(timeout=30) == 0


@pytest.fixture
def echo_port():
    """No LLM: the echo backend repeats the input, so the speech is known."""
    p, port = serve("--asr", ASR, *tts_args())
    yield port
    p.terminate()
    assert p.wait(timeout=30) == 0


class RT:
    def __init__(self, port):
        # Enter the connection as `with` would; newer websockets require it.
        self.conn = wsclient.connect(f"ws://127.0.0.1:{port}/v1/realtime", max_size=None)
        self.ws = self.conn.__enter__()
        assert json.loads(self.ws.recv(timeout=10))["type"] == "session.created"

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.conn.__exit__(*exc)

    def send(self, **ev):
        self.ws.send(json.dumps(ev))

    def until(self, type_, timeout=120):
        got, end = [], time.time() + timeout
        while True:
            ev = json.loads(self.ws.recv(timeout=max(0.1, end - time.time())))
            got.append(ev)
            if ev["type"] == "error":
                raise AssertionError(ev)
            if ev["type"] == type_:
                return got

    def append(self, pcm):
        for i in range(0, len(pcm), 48000):  # 1 s per event
            self.send(type="input_audio_buffer.append", audio=base64.b64encode(pcm[i : i + 48000]).decode())


def test_whisper_transcribes_known_speech(port):
    with RT(port) as rt:
        _transcribe(rt)


def _transcribe(rt):
    rt.send(type="session.update", session={"type": "realtime", "output_modalities": ["text"],
                                            "audio": {"input": {"turn_detection": None,
                                                                "transcription": {"model": "whisper"}}}})
    rt.until("session.updated")
    rt.append(jfk_24k())
    t0 = time.time()
    rt.send(type="input_audio_buffer.commit")
    ev = rt.until("conversation.item.input_audio_transcription.completed")[-1]
    elapsed = time.time() - t0
    w = " ".join(words(ev["transcript"]))
    assert "ask not what your country can do for you" in w, ev["transcript"]
    assert "fellow americans" in w
    print(f"\n11 s of speech transcribed in {elapsed:.2f} s: {ev['transcript']!r}")


def speak(rt, text):
    """Have the echo backend say `text`; returns (pcm16 at 24 kHz, events)."""
    rt.send(type="conversation.item.create",
            item={"type": "message", "role": "user", "content": [{"type": "input_text", "text": text}]})
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    evs = rt.until("response.done", timeout=300)
    pcm = b"".join(base64.b64decode(e["delta"]) for e in evs if e["type"] == "response.output_audio.delta")
    return pcm, evs


def transcribe(port, pcm):
    with RT(port) as rt:
        rt.send(type="session.update", session={"type": "realtime", "output_modalities": ["text"],
                                                "audio": {"input": {"turn_detection": None,
                                                                    "transcription": {"model": "w"}}}})
        rt.until("session.updated")
        rt.append(pcm)
        rt.send(type="input_audio_buffer.commit")
        return rt.until("conversation.item.input_audio_transcription.completed")[-1]["transcript"]


@needs_tts
@pytest.mark.parametrize("text", ["The quick brown fox jumps over the lazy dog.",
                                  "It is 42 degrees today."])
def test_outetts_speech_round_trips_through_whisper(echo_port, text):
    with RT(echo_port) as rt:
        t0 = time.time()
        pcm, evs = speak(rt, text)
        took = time.time() - t0
    seconds = len(pcm) / 2 / 24000
    assert 1.0 < seconds < 6.0
    heard = transcribe(echo_port, pcm)
    assert words(heard) == words(text.replace("42", "forty two")) or words(heard) == words(text), heard
    print(f"\n{seconds:.2f} s of speech synthesized in {took:.2f} s; heard {heard!r}")


@needs_tts
def test_speech_endpoint_round_trips_through_whisper(echo_port):
    text = "The quick brown fox jumps over the lazy dog."
    req = urllib.request.Request(f"http://127.0.0.1:{echo_port}/v1/audio/speech",
                                 json.dumps({"input": text}).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        assert r.headers["Content-Type"] == "audio/wav"
        with wave.open(io.BytesIO(r.read())) as w:
            assert w.getframerate() == 24000
            pcm = w.readframes(w.getnframes())
    heard = transcribe(echo_port, pcm)
    assert words(heard) == words(text), heard


@needs_tts
def test_audio_streams_while_the_sentence_is_generated(echo_port):
    text = "Streaming audio should start long before this whole sentence has been generated by the model."
    with RT(echo_port) as rt:
        rt.send(type="conversation.item.create",
                item={"type": "message", "role": "user", "content": [{"type": "input_text", "text": text}]})
        rt.until("conversation.item.done")
        t0 = time.time()
        rt.send(type="response.create")
        first, deltas = None, 0
        while True:
            ev = json.loads(rt.ws.recv(timeout=300))
            if ev["type"] == "response.output_audio.delta":
                first = first or time.time() - t0
                deltas += 1
            if ev["type"] == "response.done":
                total = time.time() - t0
                break
    assert deltas > 5 and first < 0.5 * total, (first, total)
    print(f"\nfirst audio after {first:.2f} s of {total:.2f} s")


@needs_tts
def test_cancel_interrupts_synthesis(echo_port):
    with RT(echo_port) as rt:
        rt.send(type="conversation.item.create", item={"type": "message", "role": "user", "content": [
            {"type": "input_text", "text": "This sentence is long enough to keep the speech model busy for several seconds of work."}]})
        rt.until("conversation.item.done")
        rt.send(type="response.create")
        rt.until("response.output_audio_transcript.delta")  # synthesis has started
        t0 = time.time()
        rt.send(type="response.cancel")
        done = rt.until("response.done")[-1]["response"]
        assert done["status"] == "cancelled"
        elapsed = time.time() - t0
        print(f"\ncancel took {elapsed:.2f} s")
        assert elapsed < 1.0  # polled during code generation


@needs_tts
def test_speech_to_speech(port):
    with RT(port) as rt:
        rt.send(type="session.update", session={
            "type": "realtime", "instructions": "Reply in one short sentence.",
            "audio": {"input": {"turn_detection": None, "transcription": {"model": "w"}}}})
        rt.until("session.updated")
        rt.append(jfk_24k())
        rt.send(type="input_audio_buffer.commit")
        rt.send(type="response.create")
        evs = rt.until("response.done", timeout=300)
    done = evs[-1]["response"]
    assert done["status"] == "completed"
    transcript = done["output"][0]["content"][0]["transcript"]
    pcm = b"".join(base64.b64decode(e["delta"]) for e in evs if e["type"] == "response.output_audio.delta")
    heard = transcribe(port, pcm)
    overlap = set(words(heard)) & set(words(transcript))
    assert len(overlap) >= len(set(words(transcript))) * 0.6, (transcript, heard)
    print(f"\nreply {transcript!r}; heard back {heard!r}")


def test_spoken_turn_gets_a_generated_reply(port):
    with RT(port) as rt:
        _spoken_turn(rt)


def _spoken_turn(rt):
    rt.send(type="session.update", session={
        "type": "realtime", "output_modalities": ["text"],
        "instructions": "Reply in one short sentence.",
        "audio": {"input": {"transcription": {"model": "whisper"},
                            # the speaker pauses for ~1 s between phrases
                            "turn_detection": {"type": "server_vad", "silence_duration_ms": 1500}}}})
    rt.until("session.updated")
    rt.append(jfk_24k() + bytes(48000 * 2))  # speech, then 2 s of silence
    evs = rt.until("response.done")
    types = [e["type"] for e in evs]
    assert types.count("input_audio_buffer.speech_started") == 1  # one turn, not split
    tr = [e for e in evs if e["type"] == "conversation.item.input_audio_transcription.completed"]
    assert tr and "ask not what your country" in " ".join(words(tr[0]["transcript"]))
    done = evs[-1]["response"]
    reply = done["output"][0]["content"][0]["text"]
    assert done["status"] == "completed" and len(reply.split()) >= 3, done
    stop = next(i for i, e in enumerate(evs) if e["type"] == "input_audio_buffer.speech_stopped")
    print(f"\nreply: {reply!r} (usage {done['usage']['input_tokens']} in / {done['usage']['output_tokens']} out)")
    assert stop < types.index("response.created")
