# TODO

Detail and rationale for each item are in [docs/dev/gaps.md](docs/dev/gaps.md).

## Critical

- [ ] Run a token-similarity check (JPlag) against Mongoose before the first release.

## High

- [ ] Cap the per-response output buffer so a client that stops reading cannot grow it without bound.
- [x] Prompt-prefix caching in `backends/llama` (keep the KV cache across turns).
- [ ] Multiple cache slots in `backends/llama`, so interleaved conversations do not evict each other.
- [ ] Register LLM routes so that an allocation failure in `spore_llm_new()` cannot leave routes pointing at a freed handle.
- [ ] Run `make fuzz-run` in CI with a time budget.
- [ ] Run the Autobahn WebSocket testsuite against `/ws/echo`.

- [x] Realtime phase 2: whisper.cpp `transcribe` and `spore_llama` generation behind `spore_rt_backend`, with llama.cpp's ggml shared by both.
- [x] Realtime phase 3: port OuteTTS generation (cyllama's Python loop) to C++ as `synthesize`.
- [x] Real-time TTS: measure OuteTTS-0.3-500M and GPU offload (RTX 4060: real-time factor 0.3-0.6).
- [x] Stream the TTS vocoder in overlapping windows, to cut first-audio latency per sentence.
- [x] Listening test: streamed vs whole-sentence TTS (3 sentences, no audible difference).
- [ ] Fuzz target for realtime client events.

## Medium

- [ ] Detect `POLLHUP` even when the input buffer is full.
- [ ] Keep a response alive after the client half-closes (EOF with an active response).
- [ ] Return backend error messages, so a prompt longer than the context gets a 400 that names the cause.
- [ ] Separate `<think>` output into `reasoning_content`.
- [x] Bearer token for browser WebSocket clients (via `Sec-WebSocket-Protocol`).
- [ ] Compute the poll timeout from the nearest deadline instead of capping at 1 s.

## Low

- [ ] OuteTTS speaker-profile files, so `voice` can select among them.
- [ ] Server-initiated WebSocket pings, to detect hung (not dead) local clients.
- [ ] Stream static files instead of reading them whole; add `ETag` and `Range`.
- [ ] Print embeddings at float32 precision (`%.9g`).
- [ ] Windows port.
