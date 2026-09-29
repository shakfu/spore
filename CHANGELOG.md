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

- `spore_ws`: optional WebSocket module (RFC 6455) on a new core upgrade hook. Sends are thread-safe, and `spore_ws_pending()` lets real-time producers drop frames instead of queueing when a client falls behind. Fragmentation, ping/pong, the close handshake and UTF-8 validation are handled on the loop thread. Protocol violations close with 1002, 1007 or 1009. A close frame is queued and the stream ended under one lock, so no data frame can follow it. Adds `fuzz_ws` and 30 integration tests against the `websockets` client.

### Fixed before release

- Lost wake-up in the event loop. The loop cleared `wake_pending` before draining the wake pipe. A worker byte written between the two was drained, but the flag stayed set, so no later worker wrote again. Every cross-thread reply then waited for the 1 s poll cap. `test_no_lost_wakeups_under_streaming` measures the stall.
- An absent query string was `{NULL, 0}`, so `spore_query_get()` computed `NULL + 0` (undefined behaviour). It is now an empty span at the end of the target. Found by `fuzz_http` under UBSan.
