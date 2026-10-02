"""POST /v1/audio/speech over the mock TTS (a tone of 5 ms per byte at 16 kHz,
30 ms per 4-byte chunk when the text contains "slow")."""

import http.client
import io
import json
import threading
import time
import wave

import pytest

from conftest import requires, wait_for

pytestmark = requires("realtime")

TEXT = "Hello there. Second one."  # spoken as "Hello there." + "Second one."
SECONDS = 23 * 0.005


def speak(srv, payload, timeout=10):
    c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=timeout)
    body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
    c.request("POST", "/v1/audio/speech", body, {"Content-Type": "application/json"})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, r.getheader("Content-Type"), data


def test_wav_by_default(srv):
    status, ctype, data = speak(srv, {"model": "tts-1", "input": TEXT, "voice": "alloy"})
    assert status == 200 and ctype == "audio/wav"
    with wave.open(io.BytesIO(data)) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (24000, 1, 2)
        assert abs(w.getnframes() / 24000 - SECONDS) < 0.002


def test_pcm_streams_raw_samples(srv):
    status, ctype, data = speak(srv, {"input": TEXT, "response_format": "pcm"})
    assert status == 200 and ctype == "audio/pcm"
    assert len(data) % 2 == 0 and abs(len(data) / 2 / 24000 - SECONDS) < 0.002


@pytest.mark.parametrize(
    "payload, needle",
    [
        (b"not json", "JSON object"),
        ({"input": ""}, "'input'"),
        ({"input": "x" * 4097}, "'input'"),
        ({"input": "hi", "response_format": "mp3"}, "mp3"),
        ({"input": "hi", "speed": 2}, "'speed'"),
        ({"input": "hi", "stream_format": "sse"}, "SSE"),
        ({"input": "hi", "voice": 3}, "'voice'"),
    ],
)
def test_invalid_requests(srv, payload, needle):
    status, ctype, data = speak(srv, payload)
    assert status == 400 and ctype == "application/json"
    assert needle in json.loads(data)["error"]["message"]


def test_concurrency_limit_gives_503(srv):
    slow = {"input": "slow " * 40}  # about 1.5 s each
    results = []
    threads = [threading.Thread(target=lambda: results.append(speak(srv, slow)[0])) for _ in range(5)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert sorted(results) == [200, 200, 200, 200, 503]


def test_disconnect_frees_the_slot(srv):
    socks = []
    for _ in range(4):
        c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
        c.request("POST", "/v1/audio/speech", json.dumps({"input": "slow " * 400, "response_format": "pcm"}))
        r = c.getresponse()
        r.read(64)  # audio is flowing
        socks.append((c, r))
    t = time.time()
    for c, r in socks:  # the response holds its own reference to the socket
        r.close()
        c.close()
    assert wait_for(lambda: speak(srv, {"input": "hi"})[0] == 200, timeout=1.0)
    assert time.time() - t < 1.0


def test_openai_sdk(srv):
    openai = pytest.importorskip("openai")
    c = openai.OpenAI(base_url=f"http://127.0.0.1:{srv.port}/v1", api_key="unused")
    r = c.audio.speech.create(model="tts-1", voice="alloy", input="Hi there.", response_format="wav")
    assert r.content.startswith(b"RIFF") and r.content[8:12] == b"WAVE"
