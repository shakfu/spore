# Design decisions

Each entry states the choice, then why it was preferred over the alternative.

## Local-only by construction

`spore_config` has no address field. The only choices are `ipv6` (use `::1`) and `unix_path`. A flag that allows any address could be set by mistake; a missing field cannot. The peer-address check on accept repeats the guarantee in case a future change breaks the bind.

## Host and Origin checks are in the core, not middleware

Loopback binding stops remote hosts. It does not stop a browser on the same host. Both attacks run through a hostile page:
- **DNS rebinding.** The page reaches `127.0.0.1` under its own domain, so it sends a foreign `Host`.
- **CSRF.** The page sends a cross-site `POST` with `Content-Type: text/plain`, which needs no preflight.

Both checks run before routing. So no handler can forget them. The Host check is skipped for Unix sockets, because browsers cannot connect to them.

## `poll()` rather than epoll or kqueue

`max_conns` defaults to 64. At that size `poll()` rebuilds its array in well under a microsecond. One code path then covers Linux, macOS and the BSDs. The measured cost is about 190k keep-alive req/s on one core. A backend-specific poller is worth adding only if `max_conns` must reach the thousands. For a local server that would be unusual.

## Thread-safe responses rather than loop-thread-only I/O

LLM generation takes seconds and blocks. With loop-thread-only I/O, every backend would need its own queue back to the loop. Instead, each `spore_resp` owns a mutex and an output buffer. Any thread appends to it. A self-pipe wakes the loop, which does all socket I/O. When the caller is the loop thread itself, `_Thread_local` detection skips the wake-up write. So synchronous handlers pay only an uncontended lock.

A response is reference-counted by the loop and the handler. When a connection closes, the loop sets `closed` and clears the server pointer under the response lock. So a late write from a worker cannot reach freed server memory.

## Lingering close

A reply sent while request-body bytes are still unread (413, 401 before the body) is followed by `shutdown(SHUT_WR)`. The loop then discards input until EOF or 2 s. A plain `close()` with unread data makes the kernel send RST, and the client can lose the reply. `test_413_reaches_client_despite_unread_body` covers this case.

## Strictness over leniency

| Input | Response | Reason |
|---|---|---|
| Bare LF line endings, obs-fold | 400 | Lenient parsing enables request smuggling |
| `Transfer-Encoding` on requests | 501 | Clients spore targets send `Content-Length` |
| Absolute-form targets | 400 | RFC 9112 asks servers to accept them. Rejecting them stops a request line from overriding `Host` |
| Duplicate `Content-Length` or `Host` | 400 | Ambiguous framing or authority |

## In-place JSON

The LLM layer copies each request body once and parses it in place. The parser decodes strings over their own escaped bytes, which are never shorter than the decoded result, and NUL-terminates them. Backends receive plain `const char *` with no per-string allocation.

Number parsing and printing swap the locale radix, because `strtod` and `printf` follow `LC_NUMERIC`. A host app that sets a comma locale would otherwise send `0.7` to the backend as `0`.

## Stop sequences and UTF-8 in the API layer

Token boundaries match neither stop strings nor UTF-8 sequences. Handling both in `llm.c` means each backend only emits raw bytes. The emitter holds back the longest tail that could begin a stop string, plus any incomplete UTF-8 sequence. The JSON writer replaces invalid UTF-8 with U+FFFD, so the output is always valid JSON.

## WebSockets on a generic upgrade hook

The core knows nothing about WebSocket framing. `spore__upgrade()` sends the 101 response and hands the connection's raw input to a callback. The existing response object carries all output. That object is already thread-safe and wakes the loop, so frames can come from any thread with no second queue. The module is optional in practice, not just in name. The core gains 93 lines, and `ws.c` plus `sha1.c` link only when used.

A close frame is written and the stream finished under one lock (`spore__write2(..., end)`). Checking a "closing" flag before each write would leave a gap. In that gap another thread's data frame could land after the close frame, which RFC 6455 forbids.

After sending a close frame, spore closes TCP once the frame is flushed and discards further input. It does not wait for the peer's close reply. RFC 6455 section 7.1.1 lets the server close first. Waiting would need a close timer, and the peer's reply carries nothing spore uses.

## Realtime: a pipeline behind OpenAI's event protocol

OpenAI's realtime models hear audio directly. spore has no such model, so a turn runs as transcribe, then generate, then synthesize. Each session has one worker thread that runs these steps in order, because the reply needs the transcript. A shared pool would need a per-session ordering mechanism; with at most `max_conns` sessions, a thread per session is simpler.

Both the loop thread (client events, VAD) and the worker send events and change session state, always under the session mutex. Barge-in sends `speech_started` and sets the response's cancel flag in one critical section. So `response.done` with `turn_detected` always follows `speech_started`, the order the spec requires.

Speech is synthesized one sentence at a time, as the LLM streams text. Each sentence's transcript delta is sent just before its audio. So the transcript never runs ahead of what the client can play, and a cancelled item keeps only the words the client received.

Turn detection uses 10 ms frame energy: `threshold` maps linearly onto -70..-20 dBFS, and two loud frames start speech. It is a stand-in for OpenAI's model-based VAD. The default silence is 500 ms, from the SDK docstring; the live reference shows 200 ms, which an energy detector would trip on every short pause. A model VAD (Silero, via whisper.cpp) can replace it without protocol changes.

## Engine adapters share llama.cpp's ggml

whisper.cpp and llama.cpp each ship static ggml libraries. Linking both into one binary duplicates every symbol, so ggml comes from llama.cpp only, and `libwhisper.a` links against it.

The installed whisper.cpp was built against ggml 0.23; llama.cpp's is 0.25. The mix was checked three ways:
- The ggml headers differ only in additions and in the precision API. There the enum values that matter (0 and 10) and the deprecated setters are unchanged, and `libwhisper.a` references neither.
- `struct ggml_tensor` is identical in both.
- `make test-engines` transcribes whisper.cpp's `jfk.wav` verbatim.

The alternative, rebuilding whisper.cpp from source against llama.cpp's ggml (`WHISPER_USE_SYSTEM_GGML`), removes the version skew. It is the fallback if a future ggml changes an interface whisper.cpp uses.

Both libraries were compiled with `-DGGML_MAX_NAME=160`, while the installed `ggml.h` defaults to 64. The adapters use only opaque pointers from `llama.h` and `whisper.h`, so the tensor layout never matters to them. Code that touches `struct ggml_tensor` directly must define the same value.

## OuteTTS: where the time goes

One 2.7 s sentence with the 1B model, profiled on a Ryzen 9 7940HX:

| Stage | 8 threads | 16 threads |
|---|---|---|
| Prompt, 884 tokens | 2.62 s | 1.65 s |
| Code generation, ~245 tokens | 5.9 s (42 tok/s) | 5.6 s (45 tok/s) |
| Vocoder plus inverse DFT | 0.09 s | 0.09 s |

The vocoder is negligible because the inverse DFT uses a precomputed twiddle table. The reference calls `cos` and `sin` in the inner loop, about 1.6M calls per 13 ms frame.

The prompt is re-evaluated for every sentence. OuteTTS's format puts the user text between the speaker's words and the speaker's ~800 audio codes, so a prefix cache would cover only about 60 tokens. The speaker profile length is the lever instead. With 10 words the prompt drops to 332 tokens (0.6 s), and generation rises to 49 tok/s because attention runs over less context. Whisper round trips stay verbatim at 30, 10 and 5 words.

The default thread count is the number of physical cores, 16 here. Generation is memory-bound, so it gains little; prompt evaluation is compute-bound and gains 1.6x.

The reference zeroes the first 0.25 s of every utterance against start-up artifacts. That is not done here: spore synthesizes per sentence, and the round trips keep their first words without it.

## GPU: one ggml, built by cyllama's script

The CPU build could not run OuteTTS in real time. Generation needs about 90 tok/s, and the 1B model reached about 50 (see above). On an RTX 4060, the same code and models run at a real-time factor of 0.50-0.58 (1B) and 0.28-0.30 (500M). No adapter code changed; `--gpu` sets the layer offload and whisper's `use_gpu`.

The GPU libraries come from cyllama's `manage.py`, run from a spore checkout: its paths follow the working directory, so cyllama's own build is untouched. It builds whisper.cpp and llama.cpp at the same pinned versions with one `GGML_MAX_NAME` (160). That removes the version skew noted above, and the struct-layout hazard with it. A hand-configured llama.cpp build was tried first; it needed `GGML_MAX_NAME` passed to the C, C++ and CUDA compilers separately, and its app targets failed to build. The script already encodes both.

GPU memory decides how many engine servers can run. On 8 GiB, one server with the 1B LLM, whisper and 1B TTS fits; two do not. ggml aborts on CUDA out-of-memory, so the engine tests start one server at a time.

## Streaming the vocoder

WavTokenizer maps each code to one 320-sample frame. Its decoder is not causal: a frame's spectrum depends on the codes around it, over the whole window, through attention. Streaming therefore vocodes windows: `left` codes of context, the chunk, then `ahead` codes of lookahead. Only the chunk's samples are emitted, and one frame is crossfaded into the previous window's estimate.

The parameters come from comparing streamed output with whole-sentence vocoding of identical codes (the sampling seed is fixed, so the codes match). Three sentences on an RTX 4060, with the 1B model:

| left / ahead | Waveform SNR | Spectral SNR | First audio |
|---|---|---|---|
| 4 / 2 | 8.0-9.7 dB | 13-15 dB | 0.20-0.26 s |
| 16 / 8 | 9.1-11.3 dB | 14-16 dB | 0.23-0.32 s |
| 64 / 16 (chosen) | 10.9-18.3 dB | 17-24 dB | 0.27-0.35 s |
| 1000 / 40 | 18.4-22.1 dB | 24-26 dB | - |
| whole sentence | - | - | 1.27-2.41 s |

The seam test put a one-frame window on each chunk boundary and compared error energy per sample there with the rest. For most sentences the ratio was at or below 1. A 4-frame crossfade changed nothing. So the difference is not a seam artifact. The decoder renders each window differently, because its attention sees different context. No finite window reproduces the whole-sentence rendering, and the whole-sentence rendering is itself one choice of context.

Each lookahead code costs about 7 ms of first-audio latency on the GPU. Beyond 16, the SNR gain per code is small.
