/* spore_ws: WebSocket (RFC 6455) server connections.
 * SPDX-License-Identifier: MIT
 *
 * Optional: with static linking, src/ws.c and src/sha1.c are linked only
 * if these functions are used. The upgrade request passes the same Host,
 * Origin and token checks as any other request.
 */
#ifndef SPORE_WS_H
#define SPORE_WS_H

#include "spore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spore_ws spore_ws;

enum { SPORE_WS_TEXT = 1, SPORE_WS_BINARY = 2 };

/* Callbacks run on the loop thread. Zero fields take the default shown. */
typedef struct {
    void (*on_open)(spore_ws *ws, void *ud);
    /* `data` is valid only during the call and may be modified in place
     * (e.g. by spore_json_parse). Text is validated UTF-8. Fragmented
     * messages arrive whole. */
    void (*on_message)(spore_ws *ws, int type, char *data, size_t len,
                       void *ud);
    /* Called once. `code` is the peer's close code, the code this side
     * sent if it closed first, 1005 if the peer sent none, or 1006 if the
     * connection dropped without a close. No callbacks follow. */
    void (*on_close)(spore_ws *ws, int code, void *ud);
    size_t max_message; /* 1 MiB; larger messages close with 1009 */
    const char *protocol; /* subprotocol to select when the client offers it */
} spore_ws_config;

/* In a handler, on the loop thread: complete the handshake. Returns the
 * connection, or NULL after replying with an error status. The caller owns
 * one reference and must drop it with spore_ws_release(). */
spore_ws *spore_ws_accept(spore_req *req, spore_resp *resp,
                          const spore_ws_config *cfg, void *ud);

/* Thread-safe. Return 0, or -1 once closing or closed. */
int spore_ws_send(spore_ws *ws, int type, const void *data, size_t len);
/* Send a close frame. `code` is 1000-1003, 1007-1014 or 3000-4999;
 * `reason` may be NULL and must be at most 123 bytes. */
int spore_ws_close(spore_ws *ws, int code, const char *reason);
/* Bytes queued but not yet written to the socket. A producer of real-time
 * data (audio) can drop or coarsen frames when this grows. */
size_t spore_ws_pending(spore_ws *ws);
/* Drop the caller's reference. An open connection is closed with 1000. */
void spore_ws_release(spore_ws *ws);

#ifdef __cplusplus
}
#endif
#endif
