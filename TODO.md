# TODO

Detail and rationale for each item are in [docs/dev/gaps.md](docs/dev/gaps.md). Items marked (net) are in [docs/dev/net-security.md](docs/dev/net-security.md).

## Critical

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

- [ ] (net) Reword the scope claim in `README.md`: the listener cannot be bound elsewhere, but a forwarder on the host exposes it.

- [ ] (net) `spored --token-file PATH`: generate a token, write it mode `0600`, print a URL with the token in the fragment.

- [ ] (net) Deprecate `spored --token T`, or document that `ps` exposes it to other users.

- [ ] (net) State in `README.md` which transport to use: the Unix socket for non-browser clients, TCP with a token for browsers and forwarders.

- [ ] (net) Document a reverse-proxy setup that requires `token` and keeps the Host check effective.

## Medium

- [ ] (net) Setting that disables the implicit loopback origins, so only `spore_config.origins` is accepted.

- [ ] (net) Allowed-hosts setting; apply the Host check on Unix sockets when it is set.

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

- [ ] (net) Document the Docker setup: a Unix socket in a mounted volume, or `--network host`.
