/* WebSocket connections (RFC 6455) over an upgraded spore connection.
 * SPDX-License-Identifier: MIT
 *
 * Inbound frames are parsed on the loop thread, in place in the connection
 * buffer; outbound frames go through the response's thread-safe queue.
 */
#include "spore_ws.h"
#include "internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define OP_CONT 0x0
#define OP_TEXT 0x1
#define OP_BINARY 0x2
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA
#define MAX_HEADER 14 /* 2 + 8-byte length + 4-byte mask */

struct spore_ws {
    atomic_int refs;       /* loop + caller */
    spore_resp *resp;      /* own reference */
    spore_ws_config cfg;
    void *ud;
    atomic_int close_sent; /* our close frame is queued */
    atomic_int close_code; /* code in it */
    atomic_int ended;      /* spore_end() called */
    /* loop thread only */
    int notified;
    int frag_type;         /* type of the message being reassembled, or 0 */
    spore_buf frag;
};

static void unref(spore_ws *ws) {
    if (atomic_fetch_sub(&ws->refs, 1) != 1) return;
    spore__resp_release(ws->resp);
    spore_buf_free(&ws->frag);
    free(ws);
}

static void end_once(spore_ws *ws) {
    if (!atomic_exchange(&ws->ended, 1)) spore_end(ws->resp);
}

static int send_frame(spore_ws *ws, int op, const void *data, size_t len,
                      int end) {
    unsigned char h[10];
    size_t hl = 2;
    h[0] = (unsigned char)(0x80 | op);
    if (len < 126) {
        h[1] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        h[1] = 126;
        h[2] = (unsigned char)(len >> 8);
        h[3] = (unsigned char)len;
        hl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++)
            h[2 + i] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    return spore__write2(ws->resp, h, hl, data, len, end);
}

static int valid_close_code(int code) {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) ||
           (code >= 3000 && code <= 4999);
}

int spore_ws_send(spore_ws *ws, int type, const void *data, size_t len) {
    if ((type != SPORE_WS_TEXT && type != SPORE_WS_BINARY) ||
        atomic_load(&ws->close_sent))
        return -1;
    return send_frame(ws, type == SPORE_WS_TEXT ? OP_TEXT : OP_BINARY, data,
                      len, 0);
}

int spore_ws_close(spore_ws *ws, int code, const char *reason) {
    size_t rl = reason ? strlen(reason) : 0;
    if (!valid_close_code(code) || rl > 123) return -1;
    if (atomic_exchange(&ws->close_sent, 1)) return -1;
    atomic_store(&ws->close_code, code);
    unsigned char p[125] = {(unsigned char)(code >> 8), (unsigned char)code};
    if (rl) memcpy(p + 2, reason, rl);
    if (atomic_exchange(&ws->ended, 1)) return -1; /* connection gone */
    /* Frame and end in one step: no data frame can follow the close.
     * The loop closes TCP once the frame is flushed. */
    return send_frame(ws, OP_CLOSE, p, 2 + rl, 1);
}

size_t spore_ws_pending(spore_ws *ws) { return spore__pending(ws->resp); }

void spore_ws_release(spore_ws *ws) {
    if (!atomic_load(&ws->close_sent)) spore_ws_close(ws, 1000, NULL);
    unref(ws);
}

/* ---- loop thread ------------------------------------------------------ */

static void notify(spore_ws *ws, int code) {
    if (ws->notified) return;
    ws->notified = 1;
    if (ws->cfg.on_close) ws->cfg.on_close(ws, code, ws->ud);
}

/* Fail the connection (RFC 6455 section 7.1.7). */
static void fail(spore_ws *ws, int code) {
    spore_ws_close(ws, code, NULL);
    notify(ws, code);
}

static void deliver(spore_ws *ws, int op, const char *data, size_t len) {
    if (op == OP_TEXT && !spore__utf8_valid(data, len)) {
        fail(ws, 1007);
        return;
    }
    if (ws->cfg.on_message)
        ws->cfg.on_message(ws, op == OP_TEXT ? SPORE_WS_TEXT : SPORE_WS_BINARY,
                           data, len, ws->ud);
}

static void on_close_frame(spore_ws *ws, const unsigned char *p, size_t len) {
    int code = 1005;
    if (len == 1) {
        fail(ws, 1002);
        return;
    }
    if (len >= 2) {
        code = p[0] << 8 | p[1];
        if (!valid_close_code(code)) {
            fail(ws, 1002);
            return;
        }
        if (!spore__utf8_valid((const char *)p + 2, len - 2)) {
            fail(ws, 1007);
            return;
        }
    }
    /* Echo the code; a close without one is answered with 1000. */
    spore_ws_close(ws, code == 1005 ? 1000 : code, NULL);
    notify(ws, code);
}

/* Returns the bytes consumed. Stops at an incomplete frame or once closing. */
static long feed(void *ctx, char *data, size_t len) {
    spore_ws *ws = ctx;
    size_t off = 0;
    while (!atomic_load(&ws->close_sent)) {
        const unsigned char *h = (const unsigned char *)data + off;
        size_t avail = len - off;
        if (avail < 2) break;
        int fin = h[0] & 0x80, op = h[0] & 0x0F;
        size_t hl = 2, plen = h[1] & 0x7F;
        if ((h[0] & 0x70) || !(h[1] & 0x80)) { /* RSV bits; unmasked */
            fail(ws, 1002);
            break;
        }
        if (op >= OP_CLOSE ? (op > OP_PONG || !fin || plen > 125)
                           : op > OP_BINARY) {
            fail(ws, 1002);
            break;
        }
        if (plen == 126) hl += 2;
        else if (plen == 127) hl += 8;
        if (avail < hl + 4) break;
        if (plen == 126) {
            plen = (size_t)h[2] << 8 | h[3];
        } else if (plen == 127) {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v = v << 8 | h[2 + i];
            if (v >> 63) {
                fail(ws, 1002);
                break;
            }
            plen = v > SIZE_MAX ? SIZE_MAX : (size_t)v;
        }
        if (plen > ws->cfg.max_message) {
            fail(ws, 1009);
            break;
        }
        const unsigned char *mask = h + hl;
        hl += 4;
        if (avail - hl < plen) break;
        unsigned char *p = (unsigned char *)data + off + hl;
        for (size_t i = 0; i < plen; i++) p[i] ^= mask[i & 3];
        off += hl + plen;

        switch (op) {
        case OP_TEXT:
        case OP_BINARY:
            if (ws->frag_type) {
                fail(ws, 1002);
            } else if (fin) {
                deliver(ws, op, (const char *)p, plen);
            } else {
                ws->frag_type = op;
                ws->frag.len = 0;
                spore_buf_add(&ws->frag, p, plen);
            }
            break;
        case OP_CONT:
            if (!ws->frag_type) {
                fail(ws, 1002);
                break;
            }
            if (ws->frag.len + plen > ws->cfg.max_message) {
                fail(ws, 1009);
                break;
            }
            spore_buf_add(&ws->frag, p, plen);
            if (ws->frag.err) {
                fail(ws, 1011);
            } else if (fin) {
                int t = ws->frag_type;
                ws->frag_type = 0;
                deliver(ws, t, ws->frag.ptr ? ws->frag.ptr : "", ws->frag.len);
                ws->frag.len = 0;
            }
            break;
        case OP_PING:
            send_frame(ws, OP_PONG, p, plen, 0);
            break;
        case OP_PONG:
            break;
        case OP_CLOSE:
            on_close_frame(ws, p, plen);
            break;
        }
    }
    /* Once closing, discard the rest: RFC 6455 ignores data after close. */
    return atomic_load(&ws->close_sent) ? (long)len : (long)off;
}

/* The connection is gone (or the server is being freed). */
static void gone(void *ctx) {
    spore_ws *ws = ctx;
    notify(ws, atomic_load(&ws->close_sent) ? atomic_load(&ws->close_code) : 1006);
    end_once(ws);
    unref(ws);
}

/* ---- handshake -------------------------------------------------------- */

static int valid_key(spore_str k) {
    if (k.len != 24 || k.ptr[22] != '=' || k.ptr[23] != '=') return 0;
    for (size_t i = 0; i < 22; i++) {
        char c = k.ptr[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/'))
            return 0;
    }
    return 1;
}

static spore_ws *reject(spore_resp *resp, int status, const char *extra) {
    if (extra) spore_set_header(resp, "Sec-WebSocket-Version", extra);
    const char *msg = status == 426 ? "426 Upgrade Required\n"
                    : status == 500 ? "500 Internal Server Error\n"
                                    : "400 Bad Request\n";
    spore_reply(resp, status, "text/plain", msg, strlen(msg));
    return NULL;
}

spore_ws *spore_ws_accept(spore_req *req, spore_resp *resp,
                          const spore_ws_config *cfg, void *ud) {
    spore_str key = spore_header_get(req, "Sec-WebSocket-Key");
    spore_str ver = spore_header_get(req, "Sec-WebSocket-Version");
    if (!spore_str_eq(req->method, "GET") || req->minor < 1 ||
        !spore__has_token(spore_header_get(req, "Upgrade"), "websocket") ||
        !spore__has_token(spore_header_get(req, "Connection"), "upgrade") ||
        !valid_key(key))
        return reject(resp, 400, NULL);
    if (!spore_str_eq(ver, "13")) return reject(resp, 426, "13");

    /* Sec-WebSocket-Accept = base64(SHA-1(key + GUID)), section 4.2.2 */
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[24 + sizeof guid];
    memcpy(cat, key.ptr, 24);
    memcpy(cat + 24, guid, sizeof guid - 1);
    unsigned char digest[20];
    spore__sha1(cat, sizeof cat - 1, digest);

    spore_buf h = {0};
    spore_buf_puts(&h, "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: ");
    spore__base64(&h, digest, sizeof digest);
    spore_buf_puts(&h, "\r\n");
    if (cfg && cfg->protocol &&
        spore__has_token(spore_header_get(req, "Sec-WebSocket-Protocol"),
                         cfg->protocol))
        spore_buf_printf(&h, "Sec-WebSocket-Protocol: %s\r\n", cfg->protocol);

    spore_ws *ws = calloc(1, sizeof *ws);
    if (h.err || !ws) {
        spore_buf_free(&h);
        free(ws);
        return reject(resp, 500, NULL);
    }
    if (cfg) ws->cfg = *cfg;
    if (!ws->cfg.max_message) ws->cfg.max_message = 1u << 20;
    ws->ud = ud;
    ws->resp = resp;
    atomic_init(&ws->refs, 2);
    spore__resp_retain(resp);
    spore__upgrade_hooks hooks = {feed, gone, ws};
    int rc = spore__upgrade(resp, h.ptr, &hooks, ws->cfg.max_message + MAX_HEADER);
    spore_buf_free(&h);
    if (rc) {
        spore__resp_release(resp);
        free(ws);
        return reject(resp, 500, NULL);
    }
    if (ws->cfg.on_open) ws->cfg.on_open(ws, ud);
    return ws;
}

/* ---- testing ---------------------------------------------------------- */

/* Same ownership as spore_ws_accept(): the response holds a loop and a
 * handler reference, and the ws holds a third. */
spore_ws *spore__ws_detached(const spore_ws_config *cfg, void *ud) {
    spore_ws *ws = calloc(1, sizeof *ws);
    if (!ws || !(ws->resp = spore__resp_detached())) {
        free(ws);
        return NULL;
    }
    spore__resp_retain(ws->resp);
    if (cfg) ws->cfg = *cfg;
    if (!ws->cfg.max_message) ws->cfg.max_message = 1u << 20;
    ws->ud = ud;
    atomic_init(&ws->refs, 2);
    return ws;
}

long spore__ws_feed(spore_ws *ws, char *data, size_t len) {
    return feed(ws, data, len);
}

/* Stands in for the loop when the connection goes: drop the loop's
 * response reference, then run the hook. The caller still owns its
 * reference and releases it with spore_ws_release(). */
void spore__ws_gone(spore_ws *ws) {
    spore__resp_release(ws->resp);
    gone(ws);
}
