# Known gaps

Open issues found during the 0.1.0 work. Deliberate deviations from the RFCs are in [design.md](design.md), not here.

## Security and robustness

| Gap | Effect | Suggested fix |
|---|---|---|
| Connection state machine not fuzzed | The fuzz targets cover the parsers only. Framing across reads, pipelining, `100-continue` and draining in `conn_service()` are covered only by the integration tests | A target that drives `spore_poll()` over a socketpair with fuzzed byte streams and split points |
| A closed TCP client is seen only on write | EOF is a half-close, so a TCP client that closes is noticed when a write fails. A non-streaming LLM reply keeps generating until it ends. Unix-socket peers raise `POLLHUP` and are noticed at once | None that keeps half-close: TCP cannot tell `close()` from `shutdown(SHUT_WR)` without writing |
| `POLLHUP` read as POSIX defines it | `POLLHUP` closes the connection: POSIX makes it exclusive with `POLLOUT`. If a platform raises it on a half-close alone, as macOS may, half-closed clients lose their responses there. Linux was tested; macOS was not | Test on macOS |

## HTTP server

- **Static files are read whole.** `spore_serve_dir()` loads each file into memory. It suits UI assets, not large media. Fix: stream in chunks, or use `sendfile()`.

- **No Windows support.** Porting needs `WSAPoll`, a socketpair in place of the self-pipe, and a peer-credential check for named pipes.

- **No conditional or range requests.** Static files get no `ETag`, `If-Modified-Since` or `Range` handling.

## WebSockets

- **Autobahn not yet run.** `make autobahn` and the CI job run the [Autobahn testsuite](https://github.com/crossbario/autobahn-testsuite) against `/ws/echo`, but no result has been recorded: the development host has no Docker. Messages over 1 MiB (`max_message`) and compression cases are excluded.

- **No keep-alive pings.** On loopback and Unix sockets, the kernel closes a dead process's sockets, so a vanished peer is always seen. Pings would only detect a hung process, such as one stopped in a debugger. Browsers answer pings in their network stack, so a frozen tab would still pass.

- **Send queue overflow closes.** A send past `spore_config.max_pending` closes the connection; there is no blocking send. Real-time producers should check `spore_ws_pending()` and drop frames first.

- **No extensions.** `permessage-deflate` is not offered. Compressed audio gains little from it.

- **Text validated after reassembly.** Invalid UTF-8 in a fragmented message is detected once the message is complete, not at the first bad fragment. RFC 6455 allows this.

## Realtime

- **Energy VAD.** Background noise above the threshold starts turns. `semantic_vad` is accepted but maps `eagerness` onto silence lengths (300/600/1200 ms).

- **Not implemented:** tools and function calls, G.711 (`audio/pcmu`, `audio/pcma`), transcription-only sessions, out-of-band responses (`conversation: "none"`, `response.input`), `idle_timeout_ms`, input transcription deltas, and audio in `conversation.item.retrieved` (only lengths are stored).

- **One content part per item.** Multi-part user messages are joined into one text.

- **`/v1/audio/speech` encodes only WAV and raw PCM.** OpenAI's default, `mp3`, gets 400; so do `opus`, `aac` and `flac`. An omitted `response_format` gives WAV.

- **Per-chunk resampling of synthesized audio** (realtime, and `pcm` from `/v1/audio/speech`). Chunks are resampled independently, so a TTS rate other than 24 kHz gets small discontinuities at chunk edges. OuteTTS produces 24 kHz, so no resampling happens there.

- **No rate limits.** `rate_limits.updated` is never sent. Clients treat it as optional.

- **Batch transcription.** whisper runs once the turn is committed, so its time (about 4% of the audio length with `ggml-base.en` on CPU) adds directly to response latency. There are no transcription deltas while the user speaks.

- **TTS is real-time only on a GPU.** On a Ryzen 9 7940HX, OuteTTS runs at real-time factor 1.15-1.63 (500M) and 2.6-3.1 (1B). On an RTX 4060 it runs at 0.28-0.58.

- **OuteTTS sometimes garbles a word, usually the first.** It is a sampled LLM-TTS: it can open with a pause inside the first word's duration and then compress the word. In a whisper check of 6 sentences starting with multi-syllable words, the 1B model got 5-6 of 6 first words right, varying by seed ("streaming" failed under 3 of 4 seeds). Lower temperatures moved the error to other words. The 500M model got 2-4 of 6. The effect is identical with and without streaming. Accepted as a model limit. A phoneme-based TTS (Kokoro, Piper) would avoid it.

- **Streamed TTS: first listening check done.** Streamed audio differs from whole-sentence vocoding (spectral SNR 17-24 dB), with unchanged intelligibility. An informal A/B listen found no audible difference on 3 sentences. On CPU, generation is slower than playback, so streamed audio will stall between chunks.

- **CUDA out-of-memory aborts the process.** ggml aborts rather than returning an error, so one engine server per GPU is the safe limit on 8 GiB.

- **One voice, English only.** `voice` is ignored in favour of the built-in `en_male_1` profile. Text is reduced to ASCII words, so accented letters are dropped. Speaker-profile files are not loaded yet.

- **ggml version skew.** whisper.cpp (ggml 0.23) links against llama.cpp's ggml 0.25. It is verified for the installed builds only; see `design.md`.

## LLM layer

- **Prompt-cache slots share one KV buffer.** Attention covers the cells of every slot, so filled slots slow generation (about 14% with three 2k-token slots on CPU). Separate buffers would split `n_ctx` between slots instead.

- **No context shift.** A conversation that fills `n_ctx` ends with `finish_reason: "length"`; nothing is discarded to make room.

- **No batching.** One context behind a mutex. `workers > 1` gains nothing with `spore_llama`.

- **Only a leading `<think>` block is separated.** A template that opens the block in the prompt (the reply starts inside it) is not detected. The realtime module passes the raw text to TTS, so a thinking model's reasoning would be spoken.

- **Unsupported request features:** tool calls, image and audio content, `n > 1`, logprobs, token-array inputs to embeddings.

- **Embedding errors are generic.** `embed` returns only -1, so an input longer than the batch gets 500 without a cause.

- **Embeddings are printed with 17 digits.** Float vectors go out at double precision. That matches llama-server, but `%.9g` would round-trip float32 in about half the bytes. `encoding_format: "base64"` already avoids the cost.
