# TODO

Detail and rationale for each item are in [docs/dev/gaps.md](docs/dev/gaps.md). Items marked (net) are in [docs/dev/net-security.md](docs/dev/net-security.md).

## Critical

## High

- [x] Run the Autobahn WebSocket testsuite against `/ws/echo`.

- [ ] Decide which user group is primary: C embedders, llama-server replacement users, or local voice-assistant builders. The order of the items below depends on it.

- [ ] Realtime: stop failing empty-transcript turns. A noise-triggered VAD turn (empty whisper transcript), or a response with no messages, ends with `status: "failed"` (`unheard()`, `run_response`). OpenAI defines no status for this. Its one documented no-speech path, `idle_timeout_ms`, generates a normal response. Prefer sending no `response.*` events after an empty transcript, as with `create_response: false`.

- [ ] `backends/llama`: with `n_ctx = 0` (the training context), embedding `n_batch` stays 512, so `embed()` refuses longer inputs. Use `llama_model_n_ctx_train()`. Needs `make test-engines` with an embedding model such as nomic-embed (8k context).

- [ ] Model-based VAD (Silero, which whisper.cpp ships) in place of the energy detector. Noise above the threshold starts turns. `semantic_vad` maps `eagerness` onto silence lengths only.

- [ ] A CPU-real-time TTS backend: Kokoro or Piper, both phoneme-based. OuteTTS is real-time only on a GPU and garbles some first words. CPU users are likely most local users.

- [ ] Jinja chat templates and tool calls in `spore_llm`, `backends/llama` and the realtime module. Only llama.cpp's built-in templates are used now (`llama_chat_apply_template`), so models with other templates fail, and `chat_template_kwargs` is unavailable. Most agent clients need tools. Until both exist, the `llama-server` link fails on common invocations such as `--jinja`.

## Medium

- [x] Fuzz target for the connection state machine: drive `spore_poll()` over a socketpair with fuzzed byte streams and split points.

- [ ] Test half-close and `POLLHUP` handling on macOS; POSIX makes `POLLHUP` exclusive with `POLLOUT`, but macOS may raise it on a half-close alone.

- [x] Realtime: strip `<think>` blocks before TTS, so a thinking model's reasoning is not spoken.

- [x] Detect a `<think>` block opened by the chat template (the reply starts inside it).

- [x] Return a cause for embedding failures; `embed` returns only -1, so an over-long input gets a bare 500.

- [ ] Split `src/realtime.c` (about 1,850 lines, eight jobs). Then VAD and event order can be tested without a whole session:
  - `rt_vad.c`: VAD as a pure function over frames and a small state struct.

  - `rt_proto.c`: event encoders.

  - `rt_conv.c`: conversation items.

  - `speech.c`: `POST /v1/audio/speech`.

- [ ] Direct fuzz targets, with round-trip properties, for `spore__dechunk`, `spore_url_decode`, `byte_range` (`static.c`), `spore__base64_decode` and `spore__resample`. They are now reached only through `fuzz_conn` and `fuzz_rt`.

- [ ] (net) Token from an environment variable or an inherited fd for `spore serve`, for a parent that spawns it. llama-server reads `LLAMA_API_KEY` (check its README).

- [ ] Shared library and a pkg-config file, for non-CMake consumers such as Python, Rust or Go FFI. Only `libspore.a` and a CMake package are installed now.

- [ ] Measure voice latency while chat traffic runs on the same server. The realtime pipeline and `/v1/chat/completions` share `spore_llama`'s single mutex, so a voice turn waits for unrelated generations.

- [ ] README: lead with the embeddable local server; describe the LLM layer as an adapter for OpenAI clients. Reconsider the `llama-server` link until Jinja templates and tools exist.

- [ ] `backends/llama`: guard prefix reuse with `llama_memory_seq_pos_min`, as llama-server does for sliding-window (SWA) caches, or set `swa_full = true` explicitly. Reuse is safe only while `swa_full` stays true, which is inferred to be the default.

## Low

- [ ] OuteTTS speaker-profile files, so `voice` can select among them.

- [ ] Server-initiated WebSocket pings, to detect hung (not dead) local clients.

- [ ] Stream static files instead of reading them whole; add `ETag`.

- [ ] Print embeddings at float32 precision (`%.9g`).

- [ ] Windows port.

- [ ] (net) Document the Docker setup: a Unix socket in a mounted volume, or `--network host`.

- [ ] Context shift in `backends/llama`: discard old tokens instead of ending with `finish_reason: "length"` when `n_ctx` fills. Plan it with the realtime conversation cap (below): long sessions hit both limits.

- [ ] Stateful streaming resampler, so per-chunk TTS audio at rates other than 24 kHz has no edge discontinuities.

- [ ] Streaming transcription: input transcription deltas while the user speaks, instead of one whisper pass after commit.

- [ ] Realtime: G.711 (`audio/pcmu`, `audio/pcma`), transcription-only sessions, out-of-band responses, `idle_timeout_ms`, multi-part content, and audio in `conversation.item.retrieved`.

- [ ] `/v1/audio/speech`: `mp3` and `opus` output (OpenAI's default is `mp3`).

- [ ] OuteTTS: keep accented letters; text is reduced to ASCII words.

- [ ] Document JSON node growth beside `max_body`: each 32-byte `spore_jnode` can come from 2 input bytes (`[0,0,...]`), so an 8 MiB body can cost about 128 MiB of nodes.

- [ ] Shrink a WebSocket response's `out` buffer after a burst; `flush()` keeps the peak allocation for the connection's lifetime.

- [ ] Unmask WebSocket payloads 8 bytes at a time instead of one (`feed()` in `ws.c`).

- [ ] Ignore a stray CRLF before the request line (RFC 9112 2.2); it now gets 400.

- [ ] Realtime: convert input audio to float in blocks in `run_transcribe`. With `max_sessions` set, a session still peaks at about 125 MB for 600 s of input: the PCM16 buffer plus full float and 16 kHz copies.

- [ ] Evaluate a single-threaded realtime session: protocol state on the loop thread, with backends posting results back as messages. Event order would then follow from message order rather than from `s->mu` held across three threads, and tests could replay any interleaving.

- [ ] Segment-aware prefix routes: `/static*` also matches `/staticfoo`.

- [ ] `flush()` holds `resp->mu` across `send()`, so producer threads wait for each send. Swap the unsent buffer out under the lock instead.

- [ ] JSON number I/O calls `localeconv()`, which races a host thread calling `setlocale` and ignores `uselocale`. Use `strtod_l`/`snprintf_l` or a locale-free formatter.

- [ ] CI: `scan-build` or `clang --analyze`, and `cppcheck`; pin actions by SHA; consider applying to OSS-Fuzz.

- [ ] Before 1.0: a `size` field or another extension convention for `spore_config` and the other config structs, so new fields do not break callers.

- [ ] Before 1.0: room in `spore_rt_backend` for an end-to-end speech model (audio in, audio out), beside the ASR, LLM and TTS stages.

- [ ] JSON-lines output for `spore serve --log`; `spore_access` already has the fields.

- [ ] `backends/llama`: parse gpt-oss's analysis and final channels into `reasoning_content`; only a leading `<think>` block is split now.

- [ ] `response_format` `json_schema` and `json_object` through grammar-constrained sampling, and `logprobs`; both get 400 now.

- [ ] Write the refuse-or-ignore rule for unsupported request fields in `docs/dev/design.md`. Refused fields get 400; the penalties stay ignored because clients send them by default (`gaps.md`).

## Beyond one local user

Needed before serving through a forwarder or proxy, or to several users. spore's scope is the local machine; these items widen it.

- [ ] Test the README's nginx configuration in CI: TLS termination, `Host` and `Origin` pass-through, WebSocket upgrade, unbuffered SSE. spore has no TLS of its own.

- [ ] Bound a whole reply's memory: `spore_reply()` queues any size, and the stall timeout only limits how long it is held. Streaming static files (above) covers the largest case.

- [ ] Survive CUDA out-of-memory: ggml aborts the process. Document a supervisor (a systemd unit with `Restart=on-failure`), or check free VRAM at load.

- [ ] Batch concurrent requests in `backends/llama`: one context behind a mutex serves one generation at a time, so `workers > 1` gains nothing.

- [ ] Cap a realtime conversation's items and text, with an error the client can act on; `find_item` is a linear scan.

- [ ] Add the soak test to the nightly CI: realtime and HTTP traffic against an ASan build, with RSS sampled over time.

- [x] Release process: tag 0.1.0, and keep the CHANGELOG's released sections fixed from then on.
