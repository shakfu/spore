# TODO

Detail and rationale for each item are in [docs/dev/gaps.md](docs/dev/gaps.md). Items marked (net) are in [docs/dev/net-security.md](docs/dev/net-security.md).

## Critical

## High

- [x] Cap the per-response output buffer so a client that stops reading cannot grow it without bound.

- [x] Prompt-prefix caching in `backends/llama` (keep the KV cache across turns).

- [x] Multiple cache slots in `backends/llama`, so interleaved conversations do not evict each other.

- [x] Register LLM routes so that an allocation failure in `spore_llm_new()` cannot leave routes pointing at a freed handle.

- [x] Run `make fuzz-run` in CI with a time budget.

- [ ] Run the Autobahn WebSocket testsuite against `/ws/echo`.

- [x] Realtime phase 2: whisper.cpp `transcribe` and `spore_llama` generation behind `spore_rt_backend`, with llama.cpp's ggml shared by both.

- [x] Realtime phase 3: port OuteTTS generation (cyllama's Python loop) to C++ as `synthesize`.

- [x] Real-time TTS: measure OuteTTS-0.3-500M and GPU offload (RTX 4060: real-time factor 0.3-0.6).

- [x] Stream the TTS vocoder in overlapping windows, to cut first-audio latency per sentence.

- [x] Listening test: streamed vs whole-sentence TTS (3 sentences, no audible difference).

- [x] Fuzz target for realtime client events.

- [x] (net) Reword the scope claim in `README.md`: the listener cannot be bound elsewhere, but a forwarder on the host exposes it.

- [x] (net) `spored --token-file PATH`: generate a token, write it mode `0600`, print a URL with the token in the fragment.

- [x] (net) Deprecate `spored --token T`, or document that `ps` exposes it to other users.

- [x] (net) State in `README.md` which transport to use: the Unix socket for non-browser clients, TCP with a token for browsers and forwarders.

- [x] (net) Document a reverse-proxy setup that requires `token` and keeps the Host check effective.

## Medium

- [x] (net) Setting that disables the implicit loopback origins, so only `spore_config.origins` is accepted.

- [x] (net) Allowed-hosts setting; apply the Host check on Unix sockets when it is set.

- [x] Detect `POLLHUP` even when the input buffer is full.

- [x] Keep a response alive after the client half-closes (EOF with an active response).

- [x] Return backend error messages, so a prompt longer than the context gets a 400 that names the cause.

- [x] Separate `<think>` output into `reasoning_content`.

- [x] Bearer token for browser WebSocket clients (via `Sec-WebSocket-Protocol`).

- [x] Compute the poll timeout from the nearest deadline instead of capping at 1 s.

- [x] `POST /v1/audio/speech`: standalone TTS over the existing `spore_rt_tts` interface, returning WAV or raw PCM.

- [ ] Fuzz target for the connection state machine: drive `spore_poll()` over a socketpair with fuzzed byte streams and split points.

- [ ] Test half-close and `POLLHUP` handling on macOS; POSIX makes `POLLHUP` exclusive with `POLLOUT`, but macOS may raise it on a half-close alone.

- [ ] Realtime: strip `<think>` blocks before TTS, so a thinking model's reasoning is not spoken.

- [ ] Detect a `<think>` block opened by the chat template (the reply starts inside it).

- [ ] Return a cause for embedding failures; `embed` returns only -1, so an over-long input gets a bare 500.

## Low

- [ ] OuteTTS speaker-profile files, so `voice` can select among them.

- [ ] Server-initiated WebSocket pings, to detect hung (not dead) local clients.

- [ ] Stream static files instead of reading them whole; add `ETag` and `Range`.

- [ ] Print embeddings at float32 precision (`%.9g`).

- [ ] Windows port.

- [ ] (net) Document the Docker setup: a Unix socket in a mounted volume, or `--network host`.

- [ ] Tool calls in `spore_llm` and the realtime module.

- [ ] Context shift in `backends/llama`: discard old tokens instead of ending with `finish_reason: "length"` when `n_ctx` fills.

- [ ] Stateful streaming resampler, so per-chunk TTS audio at rates other than 24 kHz has no edge discontinuities.

- [ ] Streaming transcription: input transcription deltas while the user speaks, instead of one whisper pass after commit.

- [ ] Model-based VAD in place of the energy detector; `semantic_vad` maps `eagerness` onto silence lengths only.

- [ ] Realtime: G.711 (`audio/pcmu`, `audio/pcma`), transcription-only sessions, out-of-band responses, `idle_timeout_ms`, multi-part content, and audio in `conversation.item.retrieved`.

- [ ] `/v1/audio/speech`: `mp3` and `opus` output (OpenAI's default is `mp3`).

- [ ] OuteTTS: keep accented letters; text is reduced to ASCII words.
