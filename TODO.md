# TODO

Detail and rationale for each item are in [docs/dev/gaps.md](docs/dev/gaps.md). Items marked (net) are in [docs/dev/net-security.md](docs/dev/net-security.md).

## Critical

## High

- [x] Run the Autobahn WebSocket testsuite against `/ws/echo`.

## Medium

- [x] Fuzz target for the connection state machine: drive `spore_poll()` over a socketpair with fuzzed byte streams and split points.

- [ ] Test half-close and `POLLHUP` handling on macOS; POSIX makes `POLLHUP` exclusive with `POLLOUT`, but macOS may raise it on a half-close alone.

- [x] Realtime: strip `<think>` blocks before TTS, so a thinking model's reasoning is not spoken.

- [x] Detect a `<think>` block opened by the chat template (the reply starts inside it).

- [x] Return a cause for embedding failures; `embed` returns only -1, so an over-long input gets a bare 500.

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
