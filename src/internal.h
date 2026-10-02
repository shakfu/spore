/* Internal declarations shared between spore modules and unit tests.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_INTERNAL_H
#define SPORE_INTERNAL_H

#include "spore.h"

#include <stdint.h>
#include "spore_ws.h"
#include "spore_realtime.h"

/* Parse a request head from buf[0..len). `scan` carries the search offset
 * for the blank line across calls; start it at 0. Returns the head length
 * including the blank line, 0 if more bytes are needed, or -status. */
long spore__parse_head(const char *buf, size_t len, size_t max_header,
                       size_t *scan, spore_req *req);

int spore__ieq(spore_str s, const char *cstr);          /* ASCII case-fold */
int spore__has_token(spore_str list, const char *tok);  /* "a, b, c" list */
/* Loopback names, or a name in `extra` (any port, ASCII case-fold). */
int spore__host_allowed(spore_str host, const char *const *extra);
/* An origin in `extra`, or with `loopback`, any http(s) loopback origin. */
int spore__origin_allowed(spore_str origin, const char *const *extra,
                          int loopback);

/* Remove every route registered with `ud`, keeping the order of the rest.
 * Lets a module undo a partial registration. Loop thread only. */
void spore__unroute(spore_server *srv, void *ud);

/* ---- connection upgrade (server.c) ----------------------------------- */

/* After an upgrade the connection stops parsing HTTP. on_data runs on the
 * loop thread with all buffered input and returns the bytes it consumed,
 * or -1 to drop the connection. on_close runs once when the connection is
 * gone, including during spore_free(). */
typedef struct {
    long (*on_data)(void *ctx, char *data, size_t len);
    void (*on_close)(void *ctx);
    void *ctx;
} spore__upgrade_hooks;

/* From inside a handler, on the loop thread: send a 101 response with
 * `extra` header lines and switch the connection to raw mode. `resp` stays
 * open for raw writes (spore_write, spore__write2) until spore_end().
 * `max_in` bounds buffered input. Returns 0, or -1 when not in a handler. */
int spore__upgrade(spore_resp *resp, const char *extra,
                   const spore__upgrade_hooks *hooks, size_t max_in);

void spore__resp_retain(spore_resp *resp);
void spore__resp_release(spore_resp *resp);
/* Append a then b as one unit, so concurrent writers cannot interleave.
 * With `end`, also finish the response in the same step (as spore_end),
 * so no later write can follow. */
int spore__write2(spore_resp *resp, const void *a, size_t alen,
                  const void *b, size_t blen, int end);
size_t spore__pending(spore_resp *resp); /* queued, unsent bytes */
/* A response with no server or connection, in raw streaming mode, for
 * driving protocol code in tests and fuzzers. */
spore_resp *spore__resp_detached(void);
/* Serve an already connected socket, bypassing the peer check; the server
 * owns fd on success. For tests/fuzz/fuzz_conn.c. Returns 0 or -1. */
int spore__adopt(spore_server *srv, int fd);

/* ---- WebSocket test entry points (ws.c) ------------------------------ */

/* Drive the frame parser without a server; see tests/fuzz/fuzz_ws.c. */
spore_ws *spore__ws_detached(const spore_ws_config *cfg, void *ud);
long spore__ws_feed(spore_ws *ws, char *data, size_t len);
void spore__ws_gone(spore_ws *ws);

spore_resp *spore__ws_resp(spore_ws *ws);
/* Move the unsent output of a detached response to `dst`. */
void spore__resp_take(spore_resp *resp, spore_buf *dst);

/* ---- realtime test entry points (realtime.c) ------------------------- */

/* A session without a server or worker thread; see
 * tests/fuzz/fuzz_rt.c. spore__rt_event() handles one client event as the
 * loop would, then runs the jobs it queued on the calling thread. */
typedef struct spore__rt_sess spore__rt_sess;
/* spore_rt_new() without the route. */
spore_rt *spore__rt_detached(const spore_rt_backend *be,
                             const spore_rt_config *cfg);
spore__rt_sess *spore__rt_open(spore_rt *rt);
void spore__rt_event(spore__rt_sess *s, char *data, size_t len);
spore_ws *spore__rt_ws(spore__rt_sess *s);
void spore__rt_close(spore__rt_sess *s);

/* ---- shared helpers --------------------------------------------------- */

void spore__base64(spore_buf *b, const unsigned char *p, size_t n);
int spore__utf8_valid(const char *s, size_t n);           /* json.c */
/* Move `end` back (not below `start`) so s[..end) does not end inside a
 * UTF-8 sequence. For streaming text in fragments. */
size_t spore__utf8_cut(const char *s, size_t start, size_t end);
void spore__sha1(const void *data, size_t len, unsigned char out[20]);

/* ---- reasoning split (think.c; llm and realtime) ---------------------- */

/* Splits a leading <think> block, as Qwen3 emits, from the reply, with the
 * whitespace around it trimmed. A zeroed struct detects the block;
 * SPORE__THINK_CONTENT passes everything as content. */
enum { SPORE__THINK_DETECT, SPORE__THINK_IN_TRIM, SPORE__THINK_IN,
       SPORE__THINK_OUT_TRIM, SPORE__THINK_CONTENT };
typedef struct {
    int mode;
    size_t sent; /* text consumed so far */
} spore__think;
/* First occurrence of n[0..nl) in h[0..hl), or NULL. */
const char *spore__find(const char *h, size_t hl, const char *n, size_t nl);
typedef void (*spore__think_fn)(void *ctx, int think, const char *p, size_t n);
/* Pass a[sent..to) to `fn`, as reasoning (think = 1) or content. Unless
 * `final`, text that may still be (part of) a tag, or whitespace to trim,
 * is held back. */
void spore__think_advance(spore__think *t, const char *a, size_t to, int final,
                          spore__think_fn fn, void *ctx);

/* ---- realtime audio (rt_audio.c) ------------------------------------- */

/* Decode base64 in place; returns the decoded length, or -1. */
long spore__base64_decode(char *s, size_t n);
void spore__pcm16_to_float(const int16_t *in, size_t n, float *out);
void spore__float_to_pcm16le(const float *in, size_t n, unsigned char *out);
/* Resample a whole buffer; returns malloc'd samples (count in *out_n). */
float *spore__resample(const float *in, size_t n, int from, int to,
                       size_t *out_n);

#endif
