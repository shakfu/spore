/* Internal declarations shared between spore modules and unit tests.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_INTERNAL_H
#define SPORE_INTERNAL_H

#include "spore.h"
#include "spore_ws.h"

/* Parse a request head from buf[0..len). `scan` carries the search offset
 * for the blank line across calls; start it at 0. Returns the head length
 * including the blank line, 0 if more bytes are needed, or -status. */
long spore__parse_head(const char *buf, size_t len, size_t max_header,
                       size_t *scan, spore_req *req);

int spore__ieq(spore_str s, const char *cstr);          /* ASCII case-fold */
int spore__has_token(spore_str list, const char *tok);  /* "a, b, c" list */
int spore__host_allowed(spore_str host);
int spore__origin_allowed(spore_str origin, const char *const *extra);

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

/* ---- WebSocket test entry points (ws.c) ------------------------------ */

/* Drive the frame parser without a server; see tests/fuzz/fuzz_ws.c. */
spore_ws *spore__ws_detached(const spore_ws_config *cfg, void *ud);
long spore__ws_feed(spore_ws *ws, char *data, size_t len);
void spore__ws_gone(spore_ws *ws);

/* ---- shared helpers --------------------------------------------------- */

void spore__base64(spore_buf *b, const unsigned char *p, size_t n);
int spore__utf8_valid(const char *s, size_t n);           /* json.c */
void spore__sha1(const void *data, size_t len, unsigned char out[20]);

#endif
