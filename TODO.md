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

- [ ] Stream static files instead of reading them whole; add `ETag`.

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

- [ ] Document JSON node growth beside `max_body`: each 32-byte `spore_jnode` can come from 2 input bytes (`[0,0,...]`), so an 8 MiB body can cost about 128 MiB of nodes.

- [ ] Shrink a WebSocket response's `out` buffer after a burst; `flush()` keeps the peak allocation for the connection's lifetime.

- [ ] Unmask WebSocket payloads 8 bytes at a time instead of one (`feed()` in `ws.c`).

- [ ] Ignore a stray CRLF before the request line (RFC 9112 2.2); it now gets 400.

## Beyond one local user

Needed before serving through a forwarder or proxy, or to several users. spore's scope is the local machine; these items widen it.

- [ ] Test the README's nginx configuration in CI: TLS termination, `Host` and `Origin` pass-through, WebSocket upgrade, unbuffered SSE. spore has no TLS of its own.

- [ ] Bound a whole reply's memory: `spore_reply()` queues any size, and the stall timeout only limits how long it is held. Streaming static files (above) covers the largest case.

- [ ] Survive CUDA out-of-memory: ggml aborts the process. Document a supervisor (a systemd unit with `Restart=on-failure`), or check free VRAM at load.

- [ ] Batch concurrent requests in `backends/llama`: one context behind a mutex serves one generation at a time, so `workers > 1` gains nothing.

- [ ] A phoneme-based TTS backend (Kokoro or Piper): OuteTTS is real-time only on a GPU and garbles some first words.

- [ ] Cap a realtime conversation's items and text, with an error the client can act on; `find_item` is a linear scan.

- [ ] Add the soak test to the nightly CI: realtime and HTTP traffic against an ASan build, with RSS sampled over time.

- [ ] Release process: tag 0.1.0, and keep the CHANGELOG's released sections fixed from then on.
