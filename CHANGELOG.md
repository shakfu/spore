# Changelog

## 0.1.0 - unreleased

Initial release.

- HTTP/1.1 server restricted to loopback TCP or a Unix socket, with keep-alive, pipelining, `Expect: 100-continue`, chunked and SSE responses. Responses can be completed from any thread.

- Access policy: `Host` allowlist (DNS rebinding), `Origin` check with automatic CORS preflight, optional bearer token, peer-uid check on Unix sockets.

- Static file serving; in-place JSON reader and writers.

- `spore_llm`: OpenAI-compatible chat, completion, embedding, model and health endpoints over a backend vtable, with stop sequences, UTF-8-safe streaming, a worker pool and cancellation on disconnect.

- `backends/llama`: llama.cpp backend; `spored` demo server.

- libFuzzer targets for the HTTP head parser and the JSON reader and writers (`make fuzz`, `make fuzz-run`). Each checks properties, not just crashes: incremental and one-shot parsing agree, every span stays inside the head, JSON re-serialisation keeps its shape, and valid UTF-8 round-trips through the string writer. `make test` replays the seed corpus without clang.

- Prompt caching in `backends/llama`. The KV cache persists between requests, and only the prompt suffix that differs from the previous request is evaluated. On a 3.7k-token prompt with Qwen3-0.6B on 4 cores, a repeated request drops from 13 s to 0.45 s. Reused tokens are reported as `usage.prompt_tokens_details.cached_tokens`. Cached and uncached greedy runs can differ, because logits depend on batch shape (the same caveat as llama-server's `cache_prompt`). A request can send `"cache_prompt": false` to opt out.

- `spore_ws`: optional WebSocket module (RFC 6455) on a new core upgrade hook. Sends are thread-safe, and `spore_ws_pending()` lets real-time producers drop frames instead of queueing when a client falls behind. Fragmentation, ping/pong, the close handshake and UTF-8 validation are handled on the loop thread. Protocol violations close with 1002, 1007 or 1009. A close frame is queued and the stream ended under one lock, so no data frame can follow it. Adds `fuzz_ws` and 33 integration tests against the `websockets` client. `/ws/duplex` in `spored` demonstrates full-duplex audio with a bounded inbound queue and barge-in.

- Compile-time module selection: CMake options `SPORE_WS` and `SPORE_LLM`, or `make MODULES="ws llm"`. The core (HTTP, access policy, static files, JSON) is always built, and a build without a module contains none of its code. `make check-modules` builds and tests every module set.

- CMake build (3.21+). The `Makefile` is now only a frontend to `cmake` and `ctest`. Other projects can consume spore via `add_subdirectory` or `find_package(spore)` and link `spore::spore`, which exports the enabled modules as compile definitions. Tests and examples build only when spore is the top-level project.

- `spore_realtime`: the OpenAI Realtime API (GA) at `GET /v1/realtime`, as the optional module `SPORE_REALTIME` (needs `SPORE_WS`). It runs speech to speech as a backend pipeline: transcribe, generate (any `spore_llm_backend`), then synthesize sentence by sentence. A per-session worker keeps the steps in order, and barge-in cancels the running response. Tested against the official `openai` SDK's realtime client, the `websockets` client, and a browser-style subprotocol handshake. The mock backend in `examples/` needs no models.

- Browser authentication: with `spore_config.token` set, the token is also accepted as the WebSocket subprotocol `openai-insecure-api-key.<token>`, OpenAI's browser convention.

- `spore_ws_config.on_message` now receives `char *data`, which may be modified in place, for example by `spore_json_parse`. This avoids a copy of large messages such as 15 MiB audio appends.

- `backends/whisper`: whisper.cpp speech-to-text for `spore_realtime`. whisper.cpp and llama.cpp share llama.cpp's ggml in one binary. `spored-engines` (`make engines`) replaces `spored-llama` and combines whichever engines are configured. `make test-engines` checks a verbatim transcript of a known recording and a spoken turn answered by Llama-3.2-1B.

- `spore_rt_backend` now has one sub-struct per stage (`asr`, `tts`), each with its own function, context and rate, so independent engine adapters can be combined. Phase 1 had a single shared `self`.

- `backends/outetts`: OuteTTS 0.2/0.3 text-to-speech on llama.cpp, with the WavTokenizer vocoder, for `spore_realtime`. It is ported from cyllama's generation loop, and so from llama.cpp's OuteTTS example. Changes from those:

  - text normalization without `std::regex`;

  - an inverse DFT with a precomputed twiddle table (the vocoder takes about 0.1 s per sentence);

  - cancellation polled during code generation (`emit(ctx, NULL, 0)`);

  - a configurable speaker-profile length.

  `make test-engines` transcribes the synthesized speech back with whisper and checks the words. On a Ryzen 9 7940HX the 1B model runs 2-3x slower than real time.

- GPU engines. CMake links `libggml-cuda.a` or `libggml-vulkan.a` when `LLAMA_DIR` has them, and `spored-engines --gpu` offloads the LLM, whisper and OuteTTS. On an RTX 4060, OuteTTS runs 2-3.5x faster than real time (CPU: 1.2-3x slower). The README documents building the GPU libraries with cyllama's `manage.py`.

- OuteTTS streams: the vocoder runs over overlapping windows as codes arrive (`chunk_codes`, `--tts-chunk`; `-1` is whole sentences). First audio drops from 1.3-2.4 s to 0.27-0.35 s on an RTX 4060. The streamed audio is not bit-identical to whole-sentence vocoding (see `docs/dev/design.md`).

- Prompt evaluation in `spore_llama` and `spore_outetts` runs in batches and polls for cancellation between them. An emit with no data now polls in both backend interfaces. A cancel during an OuteTTS prompt waited 0.64 s on CPU and now takes 0.25 s. This was found through an intermittent test failure.

### Fixed before release

- Lost wake-up in the event loop. The loop cleared `wake_pending` before draining the wake pipe. A worker byte written between the two was drained, but the flag stayed set, so no later worker wrote again. Every cross-thread reply then waited for the 1 s poll cap. `test_no_lost_wakeups_under_streaming` measures the stall.

- An absent query string was `{NULL, 0}`, so `spore_query_get()` computed `NULL + 0` (undefined behaviour). It is now an empty span at the end of the target. Found by `fuzz_http` under UBSan.
