# Provenance

spore is original work under the MIT licence (`LICENSE`).

It was written as a clean-room replacement for a Mongoose-derived server (`nanosrv`, GPL-2.0-only). Mongoose and nanosrv source was not opened while spore was written. Development took place in a separate repository with no shared history.

## Sources used

| Area | Source |
|---|---|
| HTTP/1.1 syntax and semantics | [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112), [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110) |
| `localhost` names | [RFC 6761 section 6.3](https://www.rfc-editor.org/rfc/rfc6761#section-6.3) |
| Origin and CORS | [WHATWG Fetch](https://fetch.spec.whatwg.org/), [RFC 6454](https://www.rfc-editor.org/rfc/rfc6454) |
| Server-Sent Events | [WHATWG HTML section 9.2](https://html.spec.whatwg.org/multipage/server-sent-events.html) |
| JSON | [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) |
| Base64 | [RFC 4648](https://www.rfc-editor.org/rfc/rfc4648) |
| OpenAI API shapes | [OpenAI API reference](https://platform.openai.com/docs/api-reference) |
| OpenAI Realtime API (GA) | `openai` Python SDK 3.21.0 type definitions (`openai/types/realtime/`, Apache-2.0), [Realtime WebSocket guide](https://developers.openai.com/api/docs/guides/realtime-websocket), [server events reference](https://developers.openai.com/api/reference/resources/realtime/server-events); wire shapes only, no code |
| llama-server defaults | [llama.cpp server README](https://github.com/ggml-org/llama.cpp/tree/master/tools/server) (MIT) |
| llama.cpp backend | `llama.h` public API (MIT) |
| WebSocket | [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455) |
| SHA-1 | [FIPS 180-4 section 6.1](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) |
| POSIX interfaces | POSIX.1-2008 |
