# spore

A minimal HTTP/1.1 server in C11 that only serves the local machine. It includes an optional OpenAI-compatible API layer, so it can stand in for `llama-server`.

- MIT licensed, written from public specifications. See [PROVENANCE.md](PROVENANCE.md).

- POSIX only (Linux, macOS, BSD). No dependencies beyond libc and pthreads.

- 4,900 lines of C in `src/` (1,900 in the core) plus 450 lines of headers. The demo server has 112 KB of code with every module (37 KB core only) and runs in 2 MB RSS.

## Scope

spore can only listen on loopback (`127.0.0.1` or `::1`) or on a Unix socket. There is no address parameter, so the listener cannot be bound elsewhere by configuration. A forwarder on the same host still exposes it: `ssh -L`, `socat` or a reverse proxy connects from loopback and passes the peer check. Then only the token authenticates remote clients. See [docs/dev/net-security.md](docs/dev/net-security.md).

A local server's main attacker is a web page in the user's browser. So every request passes these checks before routing:

| Check | Blocks |
|---|---|
| `Host` must be `localhost`, `*.localhost`, `127.0.0.1`, `[::1]` or listed in `spore_config.hosts`. Checked on TCP, and on a Unix socket when `hosts` is set | DNS rebinding |
| `Origin`, if present, must be listed in `spore_config.origins`, or be a loopback origin unless `origins_only` is set | cross-site requests (CSRF) |
| Optional `Authorization: Bearer <token>`, compared in constant time | other local users and processes |
| Unix socket peers must have the server's effective uid; the socket is mode `0600` | other local users |
| TCP peers must have a loopback address | defence in depth |

CORS preflights from allowed origins are answered automatically. TLS is not supported: loopback traffic never leaves the host.

Loopback origins are allowed by default so a UI dev server on another port works without configuration. That also admits pages from every other local server. A deployment that knows its UI origin sets `origins_only` (`spored --origins-only --origin ...`).

Which transport to use:

| Client | Transport |
|---|---|
| Non-browser process on the host | Unix socket. The kernel checks the peer uid; a token adds nothing |
| Browser | TCP with a token |
| Anything behind a forwarder or proxy | TCP with a token |
| Any client, single-user host, no forwarder | TCP without a token is acceptable. Any local process can connect |

`spored --token-file PATH` generates a 32-byte token, writes it to `PATH` with mode `0600`, and prints `http://localhost:<port>/#token=<token>`. Browsers never send the fragment to a server, so it appears in no log or `Referer`. A page reads it and sends `Authorization: Bearer`. `spored --token T` still works, but other users can read `T` with `ps`.

### Behind a reverse proxy

The proxy passes the client's `Host` unchanged, and spore accepts the public name through `--host`. spore's Host check then still rejects DNS rebinding through the proxy. An nginx example:

```nginx
map $http_upgrade $connection_upgrade { default upgrade; '' close; }

server {
    listen 443 ssl;
    server_name spore.example.com;
    ssl_certificate     /etc/ssl/spore.pem;
    ssl_certificate_key /etc/ssl/spore.key;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_buffering off;      # stream SSE as it is produced
        proxy_read_timeout 1h;    # long-lived WebSockets
    }
}
```

```sh
spored --port 8080 --token-file ~/.spore-token \
    --host spore.example.com --origins-only --origin https://spore.example.com
```

- **Require the token.** Every remote client arrives from the proxy's loopback address.

- **Allow the public origin** with `--origin`. Browsers send `Origin` on POST requests and WebSocket upgrades, same-origin ones included. `--origins-only` drops the loopback origins, which mean nothing to a remote browser.

- **Use TCP, not the Unix socket.** nginx runs as another user, so the uid check rejects it.

The configuration has not been run against nginx in this repository's tests.

## Build and test

spore builds with CMake 3.21 or later. The `Makefile` is a thin frontend to `cmake` and `ctest`.

Modules are chosen at compile time. The core is always built. Each module adds only its own objects to `libspore.a`:

| Module | CMake option | Adds | Needs | `spored` code size |
|---|---|---|---|---|
| core | - | HTTP/1.1, access policy, static files, JSON | - | 37 KB |
| `ws` | `SPORE_WS` | WebSocket (`spore_ws.h`) | core | +17 KB |
| `llm` | `SPORE_LLM` | OpenAI-compatible chat, completions, embeddings (`spore_llm.h`) | core | +23 KB |
| `realtime` | `SPORE_REALTIME` | OpenAI Realtime API, speech to speech, `/v1/audio/speech` (`spore_realtime.h`) | `ws` | +43 KB |

Tests for modules left out are skipped. `spored --modules` lists what was built.

```sh
make                      # build/libspore.a, build/spored; MODULES="ws llm realtime" by default
make MODULES="ws"         # choose modules
make test                 # ctest: unit tests, fuzz seed replay, pytest integration (needs uv)
make asan tsan            # the same suites under sanitizers
make check-modules        # build and test every module set
make fuzz-run             # libFuzzer: HTTP, JSON, WebSocket, realtime events, connections (clang), FUZZ_TIME=60 each
make autobahn             # Autobahn WebSocket testsuite against /ws/echo (Docker)
make engines              # build/spored-engines with llama.cpp and whisper.cpp (LLAMA_DIR, WHISPER_DIR)
make test-engines         # end-to-end with real models (MODELS=dir, ASR_SAMPLE=wav)
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

Callbacks run on the loop thread. `spore_ws_send()` and `spore_ws_close()` are safe from any thread, so an audio thread can send frames directly. `spore_ws_pending()` reports queued bytes. A real-time producer can drop or coarsen frames when a client falls behind, instead of letting latency grow. `/ws/stream` in `examples/spored.c` shows this policy. A send that would queue more than `spore_config.max_pending` bytes closes the connection instead.

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

With a TTS stage, `spore_rt_new()` also serves `POST /v1/audio/speech`, OpenAI's text-to-speech endpoint. It returns 24 kHz mono 16-bit audio as `wav` (the default, sent whole) or `pcm` (raw samples, streamed as they are synthesized). The text is synthesized sentence by sentence; at most 4 requests run at once, and more get 503. `mp3`, `opus`, `aac`, `flac`, SSE streaming and `speed` other than 1 get 400. `voice` is passed to the engine, which may ignore it.

Not supported yet: tools, G.711 formats, transcription-only sessions, out-of-band responses (`conversation: "none"`), and audio in `conversation.item.retrieved`. See [docs/dev/gaps.md](docs/dev/gaps.md).

`spored` serves the mock backend in `examples/rt_mock_backend.c`: its transcripts report the audio length, it echoes the text, and it speaks a tone.

`spored-engines` replaces the mock stages with the engines it was given:

```sh
make engines
build/spored-engines --model Llama-3.2-1B-Instruct-Q8_0.gguf --asr ggml-base.en.bin \
    --tts OuteTTS-0.3-1B-Q6_K.gguf --vocoder WavTokenizer-Large-75-F16.gguf
```

| Stage | Engine | Adapter |
|---|---|---|
| ASR | whisper.cpp | `backends/whisper/` (C) |
| LLM | llama.cpp | `backends/llama/` (C++) |
| TTS | OuteTTS 0.2/0.3 + WavTokenizer on llama.cpp | `backends/outetts/` (C++) |

TTS real-time factor (synthesis time / speech duration; below 1 is faster than real time), with the full speaker profile. Measured on a Ryzen 9 7940HX with an RTX 4060 (8 GiB):

| TTS model | CPU, 16 threads | GPU (`--gpu`) |
|---|---|---|
| OuteTTS-0.3-1B Q6_K | 2.6-3.1 | 0.50-0.58 |
| OuteTTS-0.3-500M Q8_0 | 1.15-1.63 | 0.28-0.30 |

- **Only the GPU is real-time.** Whisper transcribes the synthesized speech back verbatim in every case.

- **ASR is fast even on CPU:** whisper `ggml-base.en` transcribes 11 s of speech in 0.4-0.5 s.

- **Speaker profile:** `--tts-speaker-words 10` shortens it and helps on CPU (real-time factor 2.1-2.5 for the 1B model). Voice consistency with the shorter profile is not measured.

- **Streaming:** audio streams while a sentence is generated. The vocoder runs over windows of 40 codes (16 for the first), with 64 codes of context and 16 of lookahead. On the RTX 4060, first audio arrives after 0.27-0.35 s instead of 1.3-2.4 s.

- **Streamed audio differs** from whole-sentence vocoding of the same codes: waveform SNR 11-18 dB, spectral SNR 17-24 dB. WavTokenizer's attention spans the whole window. Intelligibility is unchanged (whisper round trips). Perceived quality has not been rated by listeners. `--tts-chunk -1` restores whole-sentence synthesis.

- **Memory:** all three engines in one server use 3-4 GiB of GPU memory with the 1B models. Two such servers do not fit in 8 GiB. ggml aborts the process on a CUDA out-of-memory error.

For a GPU build, point `LLAMA_DIR` and `WHISPER_DIR` at llama.cpp and whisper.cpp built with one shared ggml (same `GGML_MAX_NAME`). cyllama's build script does this when run from a spore checkout:

```sh
mkdir -p deps && cd deps
GGML_CUDA=1 CMAKE_CUDA_ARCHITECTURES=89 python3 ~/projects/cyllama/scripts/manage.py build -l -w -D --cuda
cd .. && make engines BUILD=build-gpu LLAMA_DIR=deps/thirdparty/llama.cpp WHISPER_DIR=deps/thirdparty/whisper.cpp
build-gpu/spored-engines --gpu --model ... --asr ... --tts ... --vocoder ...
```

CMake links `libggml-cuda.a` (or `libggml-vulkan.a`) when it is present in `LLAMA_DIR`.

Model weights carry their own licenses, separate from spore's MIT code. Check the OuteTTS model cards: the 0.3 models are reported as CC BY-NC-SA 4.0 (1B) and CC BY-SA 4.0 (500M).

Engine adapters are separate CMake targets outside `libspore`. They are built only when `SPORE_LLAMA_DIR` or `SPORE_WHISPER_DIR` is set, and ggml is linked once, from llama.cpp.

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

`backends/llama/` wraps llama.cpp in 340 lines of C++:

```sh
make engines
build/spored-engines --model Qwen3-0.6B-Q8_0.gguf --port 8080
build/spored-engines --model bge-small-en-v1.5-q8_0.gguf --embedding --port 8081
```

The official `openai` Python SDK passes against both (`tests/test_llm.py`, `tests/test_llama.py`).

The backend keeps its KV cache between requests. It re-evaluates only the prompt suffix that is not already cached, and reports reused tokens in `usage.prompt_tokens_details.cached_tokens`. `spore_llama_config.n_slots` sets the number of cache slots. The library default is 1; `spored-engines` uses 4 (`--slots`). With several slots, interleaved conversations do not evict each other:

- A request extends the slot with the longest shared prefix, when that discards at most one token.

- Otherwise that prefix is copied into the least recently used slot. Copies share KV cells, so a common system prompt is stored once.

- When the context is full, other slots are evicted, least recently used first.

All slots share one `n_ctx`-sized KV buffer, and attention spans every cell in it. With three other slots holding about 2k tokens each, generation with Qwen3-0.6B on CPU drops from 67.6 to 58.0 tokens/s. `--slots 1` restores the single-cache behaviour. As in llama-server, cached and uncached runs can differ in their greedy output, because logits depend on batch shape. Send `"cache_prompt": false` for reproducible output.

A leading `<think>` block in a chat reply, as Qwen3 emits, goes to `reasoning_content`, streamed or not. Send `"reasoning_format": "none"` for the raw text. When the chat template opens the block in the prompt, `spore_llama` emits the tag first, so the reply is split the same way. The realtime module neither sends nor speaks the block.

A backend can reject a request with a message: `SPORE_LLM_INVALID` becomes a 400, `SPORE_LLM_ERROR` a 500, both with the backend's text. A streamed request gets the status too, because the stream head waits for the first event. The llama backend answers a prompt longer than the context with "the prompt has N tokens; the context holds M".

Not supported: tool calls, images, `n > 1`, logprobs, and batching across requests.

## Limits

Defaults, all set in `spore_config`:

| Setting | Default |
|---|---|
| Connections | 64 |
| Request head | 8 KiB, 32 headers |
| Request body | 8 MiB |
| Idle keep-alive | 30 s |
| Receive deadline per request | 30 s |
| Unsent streamed output per response | 16 MiB; past it the connection closes |

Chunked request bodies get 501, and absolute-form request targets get 400. A client that half-closes its connection (`shutdown(SHUT_WR)`) after sending requests still receives the responses. `spore_serve_dir()` reads each file whole, so it suits UI assets, not large media. Design decisions are in [docs/dev/design.md](docs/dev/design.md). Known gaps are in [docs/dev/gaps.md](docs/dev/gaps.md).
