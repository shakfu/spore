"""OpenAI-compatible endpoints over the echo backend."""

import base64
import http.client
import json
import struct
import threading
import time

import pytest

from conftest import requires

pytestmark = requires("llm")


def call(srv, path, payload=None, timeout=10):
    c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=timeout)
    if payload is None:
        c.request("GET", path)
    else:
        body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        c.request("POST", path, body, {"Content-Type": "application/json"})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, r.getheader("Content-Type"), data


def post(srv, path, payload, **kw):
    status, _, data = call(srv, path, payload, **kw)
    return status, json.loads(data)


def chat(text, **kw):
    return {"model": "x", "messages": [{"role": "user", "content": text}], **kw}


def stream(srv, path, payload, timeout=10):
    """Return the list of decoded SSE data payloads ("[DONE]" kept as str)."""
    status, ctype, data = call(srv, path, {**payload, "stream": True}, timeout)
    assert status == 200 and ctype == "text/event-stream"
    events = []
    for block in data.decode().split("\n\n"):
        if not block:
            continue
        lines = [ln[6:] for ln in block.split("\n") if ln.startswith("data: ")]
        d = "\n".join(lines)
        events.append(d if d == "[DONE]" else json.loads(d))
    return events


def test_health_and_models(srv):
    status, _, data = call(srv, "/health")
    assert status == 200 and json.loads(data) == {"status": "ok"}
    status, _, data = call(srv, "/v1/models")
    m = json.loads(data)
    assert m["object"] == "list" and m["data"][0]["id"] == "echo"


def test_chat_completion(srv):
    status, r = post(srv, "/v1/chat/completions", chat("hello big world"))
    assert status == 200
    assert r["object"] == "chat.completion" and r["id"].startswith("chatcmpl-")
    assert r["model"] == "echo"
    ch = r["choices"][0]
    assert ch["message"] == {"role": "assistant", "content": "hello big world"}
    assert ch["finish_reason"] == "stop"
    assert r["usage"] == {
        "prompt_tokens": 3,
        "completion_tokens": 3,
        "total_tokens": 6,
        "prompt_tokens_details": {"cached_tokens": 0},
    }


def test_completion(srv):
    status, r = post(srv, "/v1/completions", {"prompt": "one two"})
    assert status == 200 and r["object"] == "text_completion"
    assert r["choices"][0]["text"] == "one two"
    status, r = post(srv, "/v1/completions", {"prompt": ["solo"]})
    assert r["choices"][0]["text"] == "solo"


def test_max_tokens_gives_length(srv):
    for key in ("max_tokens", "max_completion_tokens"):
        _, r = post(srv, "/v1/chat/completions", chat("a b c d", **{key: 2}))
        assert r["choices"][0]["message"]["content"] == "a b"
        assert r["choices"][0]["finish_reason"] == "length"


def test_stop_sequence_spanning_tokens(srv):
    # "b c" arrives as tokens " b" and " c": the match must be held back.
    _, r = post(srv, "/v1/chat/completions", chat("a b c d", stop=["zz", "b c"]))
    assert r["choices"][0]["message"]["content"] == "a "
    assert r["choices"][0]["finish_reason"] == "stop"
    ev = stream(srv, "/v1/chat/completions", chat("a b c d", stop="b c"))
    text = "".join(e["choices"][0]["delta"].get("content", "") for e in ev[:-1])
    assert text == "a "
    assert ev[-2]["choices"][0]["finish_reason"] == "stop"


def test_stream_chat(srv):
    ev = stream(srv, "/v1/chat/completions", chat("hello big world"))
    assert ev[-1] == "[DONE]"
    assert ev[0]["choices"][0]["delta"]["role"] == "assistant"
    assert all(e["object"] == "chat.completion.chunk" for e in ev[:-1])
    assert len({e["id"] for e in ev[:-1]}) == 1
    text = "".join(e["choices"][0]["delta"].get("content", "") for e in ev[:-1])
    assert text == "hello big world"
    assert ev[-2]["choices"][0] == {"index": 0, "delta": {}, "finish_reason": "stop"}


def test_stream_completion_with_usage(srv):
    ev = stream(
        srv, "/v1/completions", {"prompt": "x y", "stream_options": {"include_usage": True}}
    )
    assert "".join(e["choices"][0]["text"] for e in ev[:-2]) == "x y"
    assert ev[-2]["choices"] == [] and ev[-2]["usage"]["completion_tokens"] == 2


def test_stream_never_splits_utf8(srv):
    text = "bytes: héllo 世界 \U0001f600"
    ev = stream(srv, "/v1/chat/completions", chat(text))
    parts = [e["choices"][0]["delta"].get("content", "") for e in ev[:-1]]
    assert "�" not in "".join(parts)
    assert "".join(parts) == text


def test_content_parts_joined(srv):
    msg = {"role": "user", "content": [{"type": "text", "text": "a"}, {"type": "text", "text": "b"}]}
    _, r = post(srv, "/v1/chat/completions", {"messages": [msg]})
    assert r["choices"][0]["message"]["content"] == "a\nb"


@pytest.mark.parametrize(
    "payload,needle",
    [
        (b"{not json", "not valid JSON"),
        (b"[]", "JSON object"),
        ({"messages": []}, "messages"),
        ({"messages": [{"content": "x"}]}, "role"),
        (chat("x", temperature="hot"), "temperature"),
        (chat("x", n=2), "'n'"),
        (chat("x", stop=[1]), "stop"),
        (chat("x", cache_prompt="yes"), "cache_prompt"),
        (chat("x", tools=[{"type": "function"}]), "tools"),
        ({"messages": [{"role": "user", "content": [{"type": "image_url"}]}]}, "text content"),
    ],
)
def test_invalid_requests(srv, payload, needle):
    status, r = post(srv, "/v1/chat/completions", payload)
    assert status == 400
    assert needle in r["error"]["message"]
    assert r["error"]["type"] == "invalid_request_error"


def test_backend_error(srv):
    status, r = post(srv, "/v1/chat/completions", chat("fail: now"))
    assert status == 500 and "failed" in r["error"]["message"]
    ev = stream(srv, "/v1/chat/completions", chat("fail: now"))
    assert "error" in ev[-1]


def test_embeddings(srv):
    status, r = post(srv, "/v1/embeddings", {"input": ["ab", "c"]})
    assert status == 200 and r["object"] == "list"
    assert [d["index"] for d in r["data"]] == [0, 1]
    assert len(r["data"][0]["embedding"]) == 8
    assert sum(r["data"][0]["embedding"]) == 2.0
    assert r["usage"]["prompt_tokens"] == 3
    _, b = post(srv, "/v1/embeddings", {"input": "ab", "encoding_format": "base64"})
    raw = base64.b64decode(b["data"][0]["embedding"])
    assert list(struct.unpack("<8f", raw)) == r["data"][0]["embedding"]


def test_embeddings_reject_token_arrays(srv):
    status, r = post(srv, "/v1/embeddings", {"input": [[1, 2]]})
    assert status == 400


def test_disconnect_cancels_generation(spawn):
    srv = spawn("--workers", "1")
    words = "slow:" + " w" * 60  # ~3 s if it ran to completion
    c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
    c.request("POST", "/v1/chat/completions", json.dumps(chat(words, stream=True)))
    r = c.getresponse()
    r.fp.readline()  # first event arrives
    c.sock.shutdown(2)
    c.close()
    t = time.time()
    status, r = post(srv, "/v1/chat/completions", chat("next"))
    assert status == 200 and r["choices"][0]["message"]["content"] == "next"
    elapsed = time.time() - t
    assert elapsed < 1.0, elapsed


def test_queue_full_gives_503(spawn):
    srv = spawn("--workers", "1", "--queue", "1")
    slow = chat("slow:" + " w" * 10)
    results = []

    def run():
        results.append(post(srv, "/v1/chat/completions", slow)[0])

    threads = [threading.Thread(target=run) for _ in range(2)]
    for th in threads:
        th.start()
        time.sleep(0.1)  # first runs, second waits in the queue
    status, r = post(srv, "/v1/chat/completions", slow)
    for th in threads:
        th.join()
    assert status == 503 and "busy" in r["error"]["message"]
    assert results == [200, 200]


def test_workers_run_in_parallel(spawn):
    srv = spawn("--workers", "3")
    slow = chat("slow:" + " w" * 10)  # ~0.55 s each
    results = []
    threads = [
        threading.Thread(target=lambda: results.append(post(srv, "/v1/chat/completions", slow)[0]))
        for _ in range(3)
    ]
    t = time.time()
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert results == [200] * 3
    assert time.time() - t < 1.2


def test_shutdown_during_generation_exits_cleanly(spawn):
    srv = spawn()
    c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
    c.request("POST", "/v1/chat/completions", json.dumps(chat("slow:" + " w" * 100, stream=True)))
    c.getresponse().fp.readline()
    t = time.time()
    assert srv.stop() == 0
    assert time.time() - t < 2


def test_openai_sdk(srv):
    openai = pytest.importorskip("openai")
    client = openai.OpenAI(base_url=f"http://127.0.0.1:{srv.port}/v1", api_key="unused")
    r = client.chat.completions.create(model="echo", messages=[{"role": "user", "content": "hi there"}])
    assert r.choices[0].message.content == "hi there"
    chunks = client.chat.completions.create(
        model="echo", messages=[{"role": "user", "content": "a b c"}], stream=True
    )
    assert "".join(c.choices[0].delta.content or "" for c in chunks if c.choices) == "a b c"
    e = client.embeddings.create(model="echo", input="ab")  # SDK asks for base64
    assert len(e.data[0].embedding) == 8
    assert client.models.list().data[0].id == "echo"


def test_no_lost_wakeups_under_streaming(spawn):
    # Each byte is one cross-thread wake-up. A lost wake-up stalls a
    # response until the loop's 1 s poll cap, so latency exposes it.
    srv = spawn("--workers", "4")
    body = chat("bytes:" + "x" * 400, stream=True)
    worst = 0.0
    for _ in range(150):
        t = time.time()
        assert call(srv, "/v1/chat/completions", body)[0] == 200
        worst = max(worst, time.time() - t)
    assert worst < 0.5, worst
