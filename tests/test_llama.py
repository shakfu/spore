"""llama.cpp backend, end to end. Opt-in: `make test-llama`.

Needs SPORED_LLAMA (the binary), SPORE_CHAT_MODEL and SPORE_EMBED_MODEL
(GGUF paths); skipped otherwise.
"""

import json
import os
import subprocess

import pytest

BIN = os.environ.get("SPORED_LLAMA")
CHAT = os.environ.get("SPORE_CHAT_MODEL")
EMBED = os.environ.get("SPORE_EMBED_MODEL")

pytestmark = pytest.mark.skipif(not BIN, reason="SPORED_LLAMA not set")


def start(*args):
    p = subprocess.Popen([BIN, "--port", "0", *args], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline()
    assert "listening" in line, line
    return p, int(line.rsplit(":", 1)[1])


@pytest.fixture(scope="module")
def chat_port():
    if not CHAT:
        pytest.skip("SPORE_CHAT_MODEL not set")
    p, port = start("--model", CHAT, "--ctx", "2048")
    yield port
    p.terminate()
    assert p.wait(timeout=10) == 0


@pytest.fixture(scope="module")
def embed_port():
    if not EMBED:
        pytest.skip("SPORE_EMBED_MODEL not set")
    p, port = start("--model", EMBED, "--embedding")
    yield port
    p.terminate()
    assert p.wait(timeout=10) == 0


def client(port):
    openai = pytest.importorskip("openai")
    return openai.OpenAI(base_url=f"http://127.0.0.1:{port}/v1", api_key="unused")


def test_chat_greedy_is_deterministic(chat_port):
    c = client(chat_port)
    msgs = [{"role": "user", "content": "Count from 1 to 5, digits only. /no_think"}]
    a = c.chat.completions.create(model="m", messages=msgs, temperature=0, max_tokens=40)
    b = c.chat.completions.create(model="m", messages=msgs, temperature=0, max_tokens=40)
    assert a.choices[0].message.content == b.choices[0].message.content
    assert "3" in a.choices[0].message.content
    assert a.usage.prompt_tokens > 0 and a.usage.completion_tokens > 0


def test_chat_stream_matches_non_stream(chat_port):
    c = client(chat_port)
    msgs = [{"role": "user", "content": "Say hello. /no_think"}]
    full = c.chat.completions.create(model="m", messages=msgs, temperature=0, max_tokens=20)
    chunks = c.chat.completions.create(
        model="m", messages=msgs, temperature=0, max_tokens=20, stream=True
    )
    text = "".join(ch.choices[0].delta.content or "" for ch in chunks if ch.choices)
    assert text == full.choices[0].message.content


def test_max_tokens_and_stop(chat_port):
    c = client(chat_port)
    r = c.completions.create(model="m", prompt="1, 2, 3, 4,", max_tokens=3, temperature=0)
    assert r.choices[0].finish_reason == "length" and r.usage.completion_tokens == 3
    r = c.completions.create(model="m", prompt="1, 2, 3, 4,", max_tokens=30, temperature=0, stop=["7"])
    assert r.choices[0].finish_reason == "stop" and "7" not in r.choices[0].text


def test_prompt_longer_than_context_fails_cleanly(chat_port):
    c = client(chat_port)
    openai = pytest.importorskip("openai")
    with pytest.raises(openai.InternalServerError):
        c.completions.create(model="m", prompt="word " * 5000, max_tokens=1)


def test_embeddings_are_normalised_and_semantic(embed_port):
    c = client(embed_port)
    e = c.embeddings.create(
        model="m", input=["a cat sat on the mat", "a kitten rested on the rug", "stock markets fell"]
    )
    v = [d.embedding for d in e.data]

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    assert abs(dot(v[0], v[0]) - 1) < 1e-3
    assert dot(v[0], v[1]) > dot(v[0], v[2])
    assert json.dumps(e.usage.model_dump())  # usage present


def test_prompt_cache_reuses_shared_prefix(chat_port):
    c = client(chat_port)
    system = {"role": "system", "content": "Facts: " + " ".join(f"f{i}={i * i}." for i in range(100))}
    turn1 = [system, {"role": "user", "content": "What is f3? /no_think"}]

    def cached(r):
        return r.usage.prompt_tokens_details.cached_tokens

    a = c.chat.completions.create(model="m", messages=turn1, temperature=0, max_tokens=8)
    # Identical prompt: all but the last token come from the cache.
    b = c.chat.completions.create(model="m", messages=turn1, temperature=0, max_tokens=8)
    assert cached(b) == b.usage.prompt_tokens - 1
    # A follow-up turn reuses at least the system prompt and first question.
    turn2 = turn1 + [
        {"role": "assistant", "content": a.choices[0].message.content},
        {"role": "user", "content": "And f4? /no_think"},
    ]
    r = c.chat.completions.create(model="m", messages=turn2, temperature=0, max_tokens=8)
    assert cached(r) >= a.usage.prompt_tokens - 8
    # Opting out evaluates everything.
    r = c.chat.completions.create(
        model="m", messages=turn2, temperature=0, max_tokens=8, extra_body={"cache_prompt": False}
    )
    assert cached(r) == 0


def test_uncached_greedy_is_reproducible(chat_port):
    c = client(chat_port)
    msgs = [{"role": "user", "content": "Name a prime above 50. /no_think"}]
    runs = [
        c.chat.completions.create(
            model="m", messages=msgs, temperature=0, max_tokens=16, extra_body={"cache_prompt": False}
        ).choices[0].message.content
        for _ in range(2)
    ]
    assert runs[0] == runs[1]
