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

spore builds with CMake 3.21 or later. The `Makefile` is a thin frontend to `cmake` and `ctest`.

Modules are chosen at compile time. The core is always built. Each module adds only its own objects to `libspore.a`:

| Module | CMake option | Adds | Needs | `spored` code size |
|---|---|---|---|---|
| core | - | HTTP/1.1, access policy, static files, JSON | - | 33 KB |
| `ws` | `SPORE_WS` | WebSocket (`spore_ws.h`) | core | +17 KB |
| `llm` | `SPORE_LLM` | OpenAI-compatible chat, completions, embeddings (`spore_llm.h`) | core | +21 KB |
| `realtime` | `SPORE_REALTIME` | OpenAI Realtime API, speech to speech (`spore_realtime.h`) | `ws` | +37 KB |

Tests for modules left out are skipped. `spored --modules` lists what was built.

```sh
make                      # build/libspore.a, build/spored; MODULES="ws llm realtime" by default
make MODULES="ws"         # choose modules
make test                 # ctest: unit tests, fuzz seed replay, pytest integration (needs uv)
make asan tsan            # the same suites under sanitizers
make check-modules        # build and test every module set
make fuzz-run             # libFuzzer on the HTTP, JSON and WebSocket parsers (clang), FUZZ_TIME=60
make llama                # build/spored-llama, LLAMA_DIR=path/to/llama.cpp
make test-llama           # end-to-end with real GGUF models, MODELS=dir
make install PREFIX=...   # headers, libspore.a, CMake package
```

The same builds without the frontend:

```sh
cmake -S . -B build -DSPORE_WS=ON -DSPORE_LLM=OFF
cmake --build build && ctest --test-dir build
```

Other options: `SPORE_SANITIZE` (for example `address,undefined`), `SPORE_FUZZ`, `SPORE_LLAMA_DIR`, and `SPORE_BUILD_TESTS` / `SPORE_BUILD_EXAMPLES`. The last two default to on only when spore is the top-level project.

To use spore from another CMake project, vendor it or install it:

```cmake
add_subdirectory(spore)          # or: find_package(spore 0.1 REQUIRED)
target_link_libraries(app PRIVATE spore::spore)
```

`spore::spore` carries `SPORE_WITH_WS`, `SPORE_WITH_LLM` and `SPORE_WITH_REALTIME` as compile definitions, so the application can `#ifdef` on the modules it was built with.

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

## WebSockets

`spore_ws.h` is optional. With static linking, `ws.o` and `sha1.o` are linked only if it is used.

```c
static void on_message(spore_ws *ws, int type, const char *data, size_t len, void *ud) {
    spore_ws_send(ws, type, data, len);          /* echo */
}
static void on_close(spore_ws *ws, int code, void *ud) { spore_ws_release(ws); }

static void upgrade(spore_req *req, spore_resp *resp, void *ud) {
    spore_ws_accept(req, resp, &(spore_ws_config){.on_message = on_message,
                                                  .on_close = on_close}, NULL);
}
```

Callbacks run on the loop thread. `spore_ws_send()` and `spore_ws_close()` are safe from any thread, so an audio thread can send frames directly. `spore_ws_pending()` reports queued bytes. A real-time producer can drop or coarsen frames when a client falls behind, instead of letting latency grow. `/ws/stream` in `examples/spored.c` shows this policy.

`/ws/duplex` shows the full-duplex pattern for speech in and audio out:
- `on_message` only copies inbound frames into a bounded queue, and a full queue drops its oldest frame.
- A worker thread processes the queue and sends results.
- A `cancel` text message (barge-in) empties the queue. No output from before the cancel is sent after its acknowledgement.

`tests/test_ws.py` checks ordering, drop accounting and barge-in under TSan.

The upgrade request passes the same Host, Origin and token checks as any other request. The Origin check matters most here, because browsers apply no CORS to WebSockets.

Measured on one core over loopback, with a Python client:

| Test | Result |
|---|---|
| Echo round trip, 640 B binary | 37 us p50, 43 us p99 |
| Producer thread to client, 640 B frames | 280k frames/s |
| Producer thread to client, 16 KB frames | 850 MB/s |

Not supported: extensions (including `permessage-deflate`) and server-initiated pings.

Browsers cannot set `Authorization` on a WebSocket. When `spore_config.token` is set, the core also accepts the token as the subprotocol `openai-insecure-api-key.<token>`, which is OpenAI's browser convention. This applies to any route. The key entry is never echoed back.

## Realtime API

`spore_rt_new(srv, &backend, &cfg)` serves `GET /v1/realtime` with the GA event protocol of the OpenAI Realtime API. The official SDK connects unmodified with `client.realtime.connect()`. Browsers connect with the subprotocols `realtime` and `openai-insecure-api-key.<token>`.

Speech to speech runs as a pipeline of three blocking backend calls, in order on a per-session worker thread:

| Step | Backend call | Audio |
|---|---|---|
| transcribe the committed turn | `transcribe` (for example whisper.cpp) | float at `asr_rate` |
| generate the reply | `llm.generate`, the same interface as `spore_llm` | - |
| speak it, sentence by sentence | `synthesize` | float at `tts_rate` |

On the wire, audio is 24 kHz mono 16-bit PCM in base64. The module resamples to and from the backend rates with a windowed-sinc filter.

Supported:
- session updates;
- `server_vad` turn detection, with an energy detector standing in for OpenAI's model-based VAD;
- barge-in: speech during a response cancels it with `turn_detected`;
- manual commit;
- conversation item create, delete, retrieve and truncate;
- text or audio output;
- `response.cancel`;
- input transcription events.

Not supported yet: tools, G.711 formats, transcription-only sessions, out-of-band responses (`conversation: "none"`), and audio in `conversation.item.retrieved`. See [docs/dev/gaps.md](docs/dev/gaps.md).

`spored` serves the mock backend in `examples/rt_mock_backend.c`: its transcripts report the audio length, it echoes the text, and it speaks a tone. Real engines come next.

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
