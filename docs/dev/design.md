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
