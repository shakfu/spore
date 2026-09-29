# spore

A minimal HTTP/1.1 server in C11 that only serves the local machine. It includes an optional OpenAI-compatible API layer, so it can stand in for `llama-server`.

- MIT licensed, written from public specifications. See [PROVENANCE.md](PROVENANCE.md).
- POSIX only (Linux, macOS, BSD). No dependencies beyond libc and pthreads.
- 2,300 lines of C in `src/` plus 330 lines of headers. The demo server has 52 KB of code and runs in 2 MB RSS.

## Scope

spore can only listen on loopback (`127.0.0.1` or `::1`) or on a Unix socket. There is no address parameter, so the listener cannot be exposed by configuration.

A local server's main attacker is a web page in the user's browser. So every request passes these checks before routing:

| Check | Blocks |
|---|---|
| `Host` must be `localhost`, `*.localhost`, `127.0.0.1` or `[::1]` (TCP only) | DNS rebinding |
| `Origin`, if present, must be a loopback origin or listed in `spore_config.origins` | cross-site requests (CSRF) |
| Optional `Authorization: Bearer <token>`, compared in constant time | other local users and processes |
| Unix socket peers must have the server's effective uid; the socket is mode `0600` | other local users |
| TCP peers must have a loopback address | defence in depth |

CORS preflights from allowed origins are answered automatically. TLS is not supported: loopback traffic never leaves the host.

## Build and test

```sh
make              # build/libspore.a, build/spored
make test         # unit tests, fuzz seed replay, pytest integration (needs uv)
make asan tsan    # the same suites under sanitizers
make fuzz-run     # libFuzzer on the HTTP and JSON parsers (clang), FUZZ_TIME=60
make llama        # build/spored-llama, LLAMA_DIR=path/to/llama.cpp
make test-llama   # end-to-end with real GGUF models, MODELS=dir
```

## Library use

```c
#include "spore.h"

static void hello(spore_req *req, spore_resp *resp, void *ud) {
    spore_reply(resp, 200, "text/plain", "hi\n", 3);
}

int main(void) {
    spore_server *srv = spore_new(&(spore_config){.port = 8080});
    spore_route(srv, "GET", "/hello", hello, NULL);
    spore_run(srv);          /* until spore_stop(), e.g. from a signal handler */
    spore_free(srv);
}
```

One thread runs the event loop and calls handlers. Each handler finishes its response once, in one of two ways:
- with `spore_reply()`, or
- with `spore_begin()`, `spore_write()` / `spore_sse()`, then `spore_end()`.

It can finish immediately, or later from any thread. Handlers never block the loop. They hand long work to their own threads, which stream results back. `spore_closed()` reports a client disconnect so that work can stop.

Other pieces:
- `spore_poll()` runs one iteration, for embedding in a host's own loop.
- `spore_serve_dir()` serves static files.
- `spore_json.h` provides an in-place JSON reader and escaping writers.

## LLM endpoints

`spore_llm_new(srv, &backend, &cfg)` registers:

| Route | Backend function |
|---|---|
| `GET /health`, `GET /v1/models` | none |
| `POST /v1/chat/completions`, `POST /v1/completions` | `generate` |
| `POST /v1/embeddings` (`float` and `base64`) | `embed` |

A backend is a blocking function that pushes text through a callback. spore runs it on worker threads (`workers`, default 1) behind a bounded queue (`queue`, default 16; 503 when full). spore handles the protocol work:
- request validation;
- SSE framing;
- `stop` sequences, including matches split across tokens;
- holding back UTF-8 sequences split across tokens;
- `max_tokens`;
- usage reporting;
- cancellation when the client disconnects.

Sampling defaults follow llama-server: temperature 0.8, top_k 40, top_p 0.95, min_p 0.05.

`backends/llama/` wraps llama.cpp in 230 lines of C++:

```sh
make llama
build/spored-llama --model Qwen3-0.6B-Q8_0.gguf --port 8080
build/spored-llama --model bge-small-en-v1.5-q8_0.gguf --embedding --port 8081
```

The official `openai` Python SDK passes against both (`tests/test_llm.py`, `tests/test_llama.py`).

The backend keeps its KV cache between requests. It re-evaluates only the prompt suffix that differs from the previous request, and reports reused tokens in `usage.prompt_tokens_details.cached_tokens`. As in llama-server, cached and uncached runs can differ in their greedy output, because logits depend on batch shape. Send `"cache_prompt": false` for reproducible output.

Not supported: tool calls, images, `n > 1`, logprobs, reasoning-content separation, and batching across requests.

## Limits

Defaults, all set in `spore_config`:

| Setting | Default |
|---|---|
| Connections | 64 |
| Request head | 8 KiB, 32 headers |
| Request body | 8 MiB |
| Idle keep-alive | 30 s |
| Receive deadline per request | 30 s |

Chunked request bodies get 501, and absolute-form request targets get 400. `spore_serve_dir()` reads each file whole, so it suits UI assets, not large media. Design decisions are in [docs/dev/design.md](docs/dev/design.md). Known gaps are in [docs/dev/gaps.md](docs/dev/gaps.md).
