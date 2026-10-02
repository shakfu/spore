"""OpenAI Realtime API (GA) over WebSocket, against the mock backend.

The mock transcribes any audio as "heard <N> ms", answers with the echo
LLM (it repeats the last message), and synthesizes a tone of 5 ms per byte.
"""

import base64
import json
import math
import struct
import time

import pytest

from conftest import requires

pytestmark = requires("realtime")
wsclient = pytest.importorskip("websockets.sync.client")
from websockets.exceptions import InvalidStatus  # noqa: E402

RATE = 24000


def tone(ms, amp=0.3, freq=440):
    n = RATE * ms // 1000
    return struct.pack(f"<{n}h", *(int(amp * 32767 * math.sin(2 * math.pi * freq * i / RATE)) for i in range(n)))


def silence(ms):
    return bytes(RATE * ms // 1000 * 2)


class RT:
    """Minimal event-level client."""

    def __init__(self, srv, query="?model=spore-test", **kw):
        # Enter the connection as `with` would; newer websockets require it.
        self.conn = wsclient.connect(f"ws://127.0.0.1:{srv.port}/v1/realtime{query}", max_size=None, **kw)
        self.ws = self.conn.__enter__()
        self.log = []

    def send(self, **ev):
        self.ws.send(json.dumps(ev))

    def recv(self, timeout=10):
        ev = json.loads(self.ws.recv(timeout=timeout))
        self.log.append(ev)
        return ev

    def until(self, type_, timeout=10):
        """Receive until an event of `type_`; return all events up to it."""
        got = []
        end = time.time() + timeout
        while True:
            ev = self.recv(timeout=max(0.1, end - time.time()))
            got.append(ev)
            if ev["type"] == type_:
                return got

    def append(self, pcm):
        for i in range(0, len(pcm), 9600):  # 200 ms per event
            self.send(type="input_audio_buffer.append", audio=base64.b64encode(pcm[i : i + 9600]).decode())

    def text_item(self, text, role="user"):
        part = "output_text" if role == "assistant" else "input_text"
        self.send(type="conversation.item.create", item={"type": "message", "role": role, "content": [{"type": part, "text": text}]})

    def close(self):
        self.conn.__exit__(None, None, None)


@pytest.fixture
def rt(srv):
    c = RT(srv)
    assert c.recv()["type"] == "session.created"
    yield c
    c.close()


def types(events):
    return [e["type"] for e in events]


def audio_ms(events):
    b = b"".join(base64.b64decode(e["delta"]) for e in events if e["type"] == "response.output_audio.delta")
    return len(b) / 2 / RATE * 1000


# ---- session ---------------------------------------------------------------


def test_session_created_defaults(srv):
    c = RT(srv)
    ev = c.recv()
    assert ev["type"] == "session.created" and ev["event_id"].startswith("event_")
    s = ev["session"]
    assert s["type"] == "realtime" and s["object"] == "realtime.session" and s["model"] == "spore-test"
    assert s["output_modalities"] == ["audio"]
    assert s["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 24000}
    assert s["audio"]["output"]["format"] == {"type": "audio/pcm", "rate": 24000}
    td = s["audio"]["input"]["turn_detection"]
    assert td["type"] == "server_vad" and td["threshold"] == 0.5 and td["prefix_padding_ms"] == 300
    assert td["create_response"] is True and td["interrupt_response"] is True
    assert s["audio"]["output"]["voice"] == "marin" and s["max_output_tokens"] == "inf"
    c.close()


def test_session_update_merges(rt):
    rt.send(type="session.update", session={"type": "realtime", "instructions": "x",
                                            "audio": {"input": {"turn_detection": None, "transcription": {"model": "whisper-1"}},
                                                      "output": {"voice": "cedar"}}})
    s = rt.recv()["session"]
    assert s["instructions"] == "x" and s["audio"]["input"]["turn_detection"] is None
    assert s["audio"]["input"]["transcription"]["model"] == "whisper-1"
    assert s["audio"]["output"]["voice"] == "cedar"
    assert s["output_modalities"] == ["audio"]  # untouched


@pytest.mark.parametrize(
    "session,param",
    [
        ({"type": "transcription"}, "session.type"),
        ({}, "session.type"),
        ({"type": "realtime", "output_modalities": ["text", "audio"]}, "session.output_modalities"),
        ({"type": "realtime", "audio": {"input": {"format": {"type": "audio/pcmu"}}}}, "session.audio"),
        ({"type": "realtime", "audio": {"output": {"format": "pcm16"}}}, "session.audio"),
        ({"type": "realtime", "max_output_tokens": 0}, "session.max_output_tokens"),
        ({"type": "realtime", "tools": [{"type": "function", "name": "f"}]}, "session.tools"),
        ({"type": "realtime", "audio": {"input": {"turn_detection": {"type": "server_vad", "threshold": 2}}}},
         "session.audio.input.turn_detection.threshold"),
    ],
)
def test_session_update_rejections(rt, session, param):
    rt.send(type="session.update", session=session, event_id="ev_1")
    ev = rt.recv()
    assert ev["type"] == "error"
    assert ev["error"]["param"] == param and ev["error"]["event_id"] == "ev_1"
    rt.send(type="session.update", session={"type": "realtime"})
    assert rt.recv()["session"]["output_modalities"] == ["audio"]  # nothing half-applied


# ---- errors ----------------------------------------------------------------


def test_bad_events(rt):
    rt.ws.send("{not json")
    assert rt.recv()["error"]["code"] == "invalid_json"
    rt.send(event_id="e9")
    ev = rt.recv()
    assert ev["error"]["code"] == "invalid_event" and ev["error"]["event_id"] == "e9"
    rt.send(type="no.such.event")
    assert rt.recv()["error"]["code"] == "invalid_event"
    rt.ws.send(b"\x00binary")
    assert rt.recv()["error"]["code"] == "invalid_event"
    rt.send(type="input_audio_buffer.append", audio="!!!")
    assert rt.recv()["error"]["param"] == "audio"
    rt.send(type="input_audio_buffer.commit")
    assert rt.recv()["error"]["code"] == "input_audio_buffer_commit_empty"
    rt.send(type="response.cancel")
    assert rt.recv()["error"]["code"] == "response_cancel_not_active"


# ---- responses -------------------------------------------------------------


def test_text_response_event_order(rt):
    rt.send(type="session.update", session={"type": "realtime", "output_modalities": ["text"]})
    rt.until("session.updated")
    rt.text_item("hello there")
    assert types(rt.until("conversation.item.done")) == ["conversation.item.added", "conversation.item.done"]
    rt.send(type="response.create")
    evs = rt.until("response.done")
    t = types(evs)
    assert t[:5] == ["response.created", "response.output_item.added", "conversation.item.added",
                     "response.content_part.added", "response.output_text.delta"]
    assert t[-5:] == ["response.output_text.done", "response.content_part.done", "response.output_item.done",
                      "conversation.item.done", "response.done"]
    assert "".join(e["delta"] for e in evs if e["type"] == "response.output_text.delta") == "hello there"
    done = evs[-1]["response"]
    assert done["status"] == "completed" and done["status_details"] is None
    assert done["output"][0]["content"] == [{"type": "output_text", "text": "hello there"}]
    assert done["usage"]["output_tokens"] == 2


def test_audio_response(rt):
    rt.text_item("say this. and this")
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    evs = rt.until("response.done")
    t = types(evs)
    assert "response.output_audio.delta" in t and "response.output_audio_transcript.delta" in t
    assert t.index("response.output_audio.done") < t.index("response.output_audio_transcript.done") < t.index("response.done")
    transcript = "".join(e["delta"] for e in evs if e["type"] == "response.output_audio_transcript.delta")
    assert transcript == "say this. and this"
    # tone: 5 ms per byte of text, resampled 16 kHz -> 24 kHz
    assert audio_ms(evs) == pytest.approx(5 * len(transcript), abs=1)
    part = [e for e in evs if e["type"] == "response.content_part.added"][0]["part"]
    assert part == {"type": "audio", "transcript": ""}
    out = evs[-1]["response"]["output"][0]
    assert out["content"] == [{"type": "output_audio", "transcript": transcript}] and out["status"] == "completed"


@pytest.mark.parametrize("modality", ["audio", "text"])
def test_think_block_is_not_sent_or_spoken(rt, modality):
    rt.text_item("chop:<think>plan. more plan</think>\n\nsay this.")
    rt.until("conversation.item.done")
    rt.send(type="response.create", response={"output_modalities": [modality]})
    evs = rt.until("response.done")
    kind = "output_audio_transcript" if modality == "audio" else "output_text"
    said = "".join(e["delta"] for e in evs if e["type"] == f"response.{kind}.delta")
    assert said == "say this."
    if modality == "audio":
        assert audio_ms(evs) == pytest.approx(5 * len(said), abs=1)
    assert evs[-1]["response"]["status"] == "completed"


def test_max_output_tokens_gives_incomplete(rt):
    rt.text_item("one two three four")
    rt.until("conversation.item.done")
    rt.send(type="response.create", response={"output_modalities": ["text"], "max_output_tokens": 2})
    done = rt.until("response.done")[-1]["response"]
    assert done["status"] == "incomplete" and done["status_details"]["reason"] == "max_output_tokens"


def test_second_response_while_active_is_refused(rt):
    rt.text_item("slow " * 30)
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    rt.send(type="response.create")
    evs = rt.until("error")
    assert evs[-1]["error"]["code"] == "conversation_already_has_active_response"
    rt.send(type="response.cancel")
    assert rt.until("response.done")[-1]["response"]["status"] == "cancelled"


def test_client_cancel(rt):
    rt.text_item("slow " * 40)
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    rt.until("response.output_audio.delta")
    rt.send(type="response.cancel")
    evs = rt.until("response.done")
    r = evs[-1]["response"]
    assert r["status"] == "cancelled" and r["status_details"] == {"type": "cancelled", "reason": "client_cancelled"}
    assert r["output"][0]["status"] == "incomplete"
    assert "response.output_audio.done" in types(evs)  # pending .done events still sent


# ---- input audio and VAD -----------------------------------------------------


def test_vad_turn_end_to_end(rt):
    rt.send(type="session.update", session={"type": "realtime", "audio": {"input": {"transcription": {"model": "x"}}}})
    rt.until("session.updated")
    rt.append(silence(1000) + tone(800) + silence(800))
    evs = rt.until("response.done")
    t = types(evs)
    order = ["input_audio_buffer.speech_started", "input_audio_buffer.speech_stopped",
             "input_audio_buffer.committed", "conversation.item.added", "conversation.item.done",
             "response.created"]
    assert [x for x in t if x in order][: len(order)] == order
    started = evs[t.index("input_audio_buffer.speech_started")]
    stopped = evs[t.index("input_audio_buffer.speech_stopped")]
    committed = evs[t.index("input_audio_buffer.committed")]
    assert 650 <= started["audio_start_ms"] <= 720  # 1000 ms minus 300 ms prefix padding
    assert 2250 <= stopped["audio_end_ms"] <= 2350  # speech end + 500 ms silence
    assert started["item_id"] == stopped["item_id"] == committed["item_id"]
    tr = [e for e in evs if e["type"] == "conversation.item.input_audio_transcription.completed"][0]
    heard = stopped["audio_end_ms"] - started["audio_start_ms"]
    assert tr["transcript"] == f"heard {heard} ms" and tr["item_id"] == committed["item_id"]
    # the echo LLM answers with the transcript, spoken
    transcript = "".join(e["delta"] for e in evs if e["type"] == "response.output_audio_transcript.delta")
    assert transcript == tr["transcript"]


def test_silence_alone_never_triggers(rt):
    rt.append(silence(3000) + tone(200, amp=0.001))  # -70 dBFS: below the threshold
    rt.send(type="input_audio_buffer.clear")
    assert types(rt.until("input_audio_buffer.cleared")) == ["input_audio_buffer.cleared"]


def test_manual_commit_without_vad(rt):
    rt.send(type="session.update", session={"type": "realtime", "output_modalities": ["text"],
                                            "audio": {"input": {"turn_detection": None}}})
    rt.until("session.updated")
    rt.append(tone(500))
    rt.send(type="input_audio_buffer.commit")
    evs = rt.until("conversation.item.done")
    assert types(evs) == ["input_audio_buffer.committed", "conversation.item.added", "conversation.item.done"]
    item = evs[-1]["item"]
    assert item["role"] == "user" and item["content"] == [{"type": "input_audio", "transcript": None}]
    rt.send(type="response.create")  # no auto response without VAD
    evs = rt.until("response.done")
    assert "".join(e["delta"] for e in evs if e["type"] == "response.output_text.delta") == "heard 500 ms"


def test_barge_in_cancels_before_response_done(rt):
    rt.text_item("slow " * 60)
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    rt.until("response.output_audio.delta")
    rt.append(tone(300))
    evs = rt.until("response.done")
    t = types(evs)
    assert t.index("input_audio_buffer.speech_started") < t.index("response.done")
    r = evs[-1]["response"]
    assert r["status"] == "cancelled" and r["status_details"]["reason"] == "turn_detected"
    # nothing from the cancelled response after its response.done
    rid = r["id"]
    rt.append(silence(700))
    later = rt.until("response.done")  # the new turn's response
    assert all(e.get("response_id") != rid for e in later)


def test_truncate_deletes_unheard_transcript(rt):
    rt.text_item("some words here")
    rt.until("conversation.item.done")
    rt.send(type="response.create")
    evs = rt.until("response.done")
    item_id = evs[-1]["response"]["output"][0]["id"]
    rt.send(type="conversation.item.truncate", item_id=item_id, content_index=0, audio_end_ms=10_000)
    assert rt.recv()["error"]["param"] == "audio_end_ms"
    rt.send(type="conversation.item.truncate", item_id=item_id, content_index=0, audio_end_ms=20)
    ev = rt.recv()
    assert ev == {**ev, "type": "conversation.item.truncated", "item_id": item_id, "audio_end_ms": 20}
    rt.send(type="conversation.item.retrieve", item_id=item_id)
    item = rt.recv()["item"]
    assert item["content"] == [{"type": "output_audio", "transcript": ""}] and item["status"] == "incomplete"


def test_items_insert_delete_retrieve(rt):
    rt.text_item("second")
    second = rt.until("conversation.item.done")[-1]["item"]["id"]
    rt.send(type="conversation.item.create", previous_item_id="root",
            item={"id": "first", "type": "message", "role": "system", "content": [{"type": "input_text", "text": "first"}]})
    evs = rt.until("conversation.item.done")
    assert evs[0]["previous_item_id"] is None and evs[0]["item"]["id"] == "first"
    rt.send(type="conversation.item.create", previous_item_id="nope", item={"type": "message", "role": "user", "content": []})
    assert rt.recv()["error"]["code"] == "item_not_found"
    rt.send(type="conversation.item.delete", item_id=second)
    assert rt.recv() == {**rt.log[-1], "type": "conversation.item.deleted", "item_id": second}
    rt.send(type="conversation.item.retrieve", item_id=second)
    assert rt.recv()["error"]["code"] == "item_not_found"


# ---- auth and clients --------------------------------------------------------


def test_browser_subprotocol_auth(spawn):
    srv = spawn("--token", "s3cret")
    url = f"ws://127.0.0.1:{srv.port}/v1/realtime"
    with wsclient.connect(url, subprotocols=["realtime", "openai-insecure-api-key.s3cret"]) as ws:
        assert ws.subprotocol == "realtime"  # the key is never echoed
        assert json.loads(ws.recv())["type"] == "session.created"
    with pytest.raises(InvalidStatus) as e:
        wsclient.connect(url, subprotocols=["realtime", "openai-insecure-api-key.wrong"])
    assert e.value.response.status_code == 401
    with pytest.raises(InvalidStatus):
        wsclient.connect(url)
    with wsclient.connect(url, additional_headers={"Authorization": "Bearer s3cret"}) as ws:
        assert json.loads(ws.recv())["type"] == "session.created"


def test_official_sdk_text_round_trip(srv):
    openai = pytest.importorskip("openai")
    c = openai.OpenAI(base_url=f"http://127.0.0.1:{srv.port}/v1", api_key="unused")
    with c.realtime.connect(model="m") as conn:
        assert conn.recv().type == "session.created"
        conn.session.update(session={"type": "realtime", "output_modalities": ["text"]})
        assert conn.recv().type == "session.updated"
        conn.conversation.item.create(item={"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi sdk"}]})
        conn.response.create()
        text = ""
        for ev in conn:
            if ev.type == "response.output_text.delta":
                text += ev.delta
            if ev.type == "response.done":
                assert ev.response.status == "completed"
                break
        assert text == "hi sdk"


def test_disconnect_mid_response_then_new_session(srv):
    c = RT(srv)
    c.recv()
    c.text_item("slow " * 80)
    c.until("conversation.item.done")
    c.send(type="response.create")
    c.until("response.output_audio.delta")
    c.close()
    c2 = RT(srv)
    assert c2.recv()["type"] == "session.created"
    c2.close()
