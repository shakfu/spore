# Known gaps

Open issues found during the 0.1.0 work. Deliberate deviations from the RFCs are in [design.md](design.md), not here.

## Security and robustness

| Gap | Effect | Suggested fix |
|---|---|---|
| Fuzzing not in CI | `make fuzz-run` exists but runs only by hand; the two targets have had about 150 s each | Run it in CI with a time budget; keep a persistent corpus |
| Connection state machine not fuzzed | The fuzz targets cover the parsers only. Framing across reads, pipelining, `100-continue` and draining in `conn_service()` are covered only by the integration tests | A target that drives `spore_poll()` over a socketpair with fuzzed byte streams and split points |
| Unbounded response buffer | `spore_write()` never refuses data. A client that stops reading while a worker streams makes `out` grow. Growth is bounded only by `max_tokens` | Return -1 (or block) above a per-response cap |
| Partial routes on OOM | If `spore_route()` fails partway through `spore_llm_new()`, the routes already added keep a pointer to the freed handle | Register all routes before any other allocation that can fail, or add route removal |
| Half-close aborts responses | A client that sends its request and then `shutdown(SHUT_WR)`s reads EOF. The loop treats this as a disconnect and cancels the response | Treat EOF as a close only when no response is active |
| Hang-up missed with a full buffer | When the input buffer is full, the loop does not read. A `POLLHUP` then goes undetected until the response ends | Check `POLLHUP` separately from reads |

## HTTP server

- **Static files are read whole.** `spore_serve_dir()` loads each file into memory. It suits UI assets, not large media. Fix: stream in chunks, or use `sendfile()`.

- **Timeouts fire up to 1 s late.** `spore_poll()` caps its wait at 1 s instead of computing the next deadline.

- **No Windows support.** Porting needs `WSAPoll`, a socketpair in place of the self-pipe, and a peer-credential check for named pipes.

- **No conditional or range requests.** Static files get no `ETag`, `If-Modified-Since` or `Range` handling.

## WebSockets

- **No Autobahn run.** Conformance rests on the integration tests, the `websockets` client and `fuzz_ws`. The [Autobahn testsuite](https://github.com/crossbario/autobahn-testsuite) needs Docker and has not been run.

- **No keep-alive pings.** On loopback and Unix sockets, the kernel closes a dead process's sockets, so a vanished peer is always seen. Pings would only detect a hung process, such as one stopped in a debugger. Browsers answer pings in their network stack, so a frozen tab would still pass.

- **Unbounded send queue.** `spore_ws_send()` never refuses data, like `spore_write()` (see above). Real-time producers should check `spore_ws_pending()`.

- **No extensions.** `permessage-deflate` is not offered. Compressed audio gains little from it.

- **Text validated after reassembly.** Invalid UTF-8 in a fragmented message is detected once the message is complete, not at the first bad fragment. RFC 6455 allows this.

## Realtime

- **No fuzz target for client events.** Event parsing uses the fuzzed JSON reader, but the session state machine and base64 decoding are covered only by integration tests. A target that feeds events to a detached session would close this.

- **Energy VAD.** Background noise above the threshold starts turns. `semantic_vad` is accepted but maps `eagerness` onto silence lengths (300/600/1200 ms).

- **Not implemented:** tools and function calls, G.711 (`audio/pcmu`, `audio/pcma`), transcription-only sessions, out-of-band responses (`conversation: "none"`, `response.input`), `idle_timeout_ms`, input transcription deltas, and audio in `conversation.item.retrieved` (only lengths are stored).

- **One content part per item.** Multi-part user messages are joined into one text.

- **Per-chunk resampling of synthesized audio.** Chunks are resampled independently, so a TTS rate other than 24 kHz gets small discontinuities at chunk edges. OuteTTS produces 24 kHz, so no resampling happens there.

- **No rate limits.** `rate_limits.updated` is never sent. Clients treat it as optional.

- **Batch transcription.** whisper runs once the turn is committed, so its time (about 4% of the audio length with `ggml-base.en` on CPU) adds directly to response latency. There are no transcription deltas while the user speaks.

- **TTS is real-time only on a GPU.** On a Ryzen 9 7940HX, OuteTTS runs at real-time factor 1.15-1.63 (500M) and 2.6-3.1 (1B). On an RTX 4060 it runs at 0.28-0.58.

- **OuteTTS sometimes garbles a word, usually the first.** It is a sampled LLM-TTS: it can open with a pause inside the first word's duration and then compress the word. In a whisper check of 6 sentences starting with multi-syllable words, the 1B model got 5-6 of 6 first words right, varying by seed ("streaming" failed under 3 of 4 seeds). Lower temperatures moved the error to other words. The 500M model got 2-4 of 6. The effect is identical with and without streaming. Accepted as a model limit. A phoneme-based TTS (Kokoro, Piper) would avoid it.

- **Streamed TTS: first listening check done.** Streamed audio differs from whole-sentence vocoding (spectral SNR 17-24 dB), with unchanged intelligibility. An informal A/B listen found no audible difference on 3 sentences. On CPU, generation is slower than playback, so streamed audio will stall between chunks.

- **CUDA out-of-memory aborts the process.** ggml aborts rather than returning an error, so one engine server per GPU is the safe limit on 8 GiB.

- **One voice, English only.** `voice` is ignored in favour of the built-in `en_male_1` profile. Text is reduced to ASCII words, so accented letters are dropped. Speaker-profile files are not loaded yet.

- **ggml version skew.** whisper.cpp (ggml 0.23) links against llama.cpp's ggml 0.25. It is verified for the installed builds only; see `design.md`.

## LLM layer

- **One prompt-cache slot.** The llama backend caches only the most recent token sequence. Two clients with interleaved conversations evict each other and fall back to full evaluation. llama-server keeps one cache per slot (`-np`).

- **No context shift.** A conversation that fills `n_ctx` ends with `finish_reason: "length"`; nothing is discarded to make room.

- **No batching.** One context behind a mutex. `workers > 1` gains nothing with `spore_llama`.

- **`<think>` blocks stay in `content`.** llama-server moves them to `reasoning_content`. The parsing depends on the model.

- **Unsupported request features:** tool calls, image and audio content, `n > 1`, logprobs, token-array inputs to embeddings.

- **Generic backend errors.** The backend API has no error message. A prompt longer than the context returns 500 "generation failed", not a 400 that names the cause.

- **Embeddings are printed with 17 digits.** Float vectors go out at double precision. That matches llama-server, but `%.9g` would round-trip float32 in about half the bytes. `encoding_format: "base64"` already avoids the cost.
