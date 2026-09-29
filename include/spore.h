/* spore: minimal HTTP/1.1 server restricted to local clients.
 * SPDX-License-Identifier: MIT
 *
 * Scope: loopback TCP (127.0.0.1 or ::1) or a Unix socket, nothing else.
 * One event-loop thread owns all sockets. Responses may be completed from
 * any thread; see spore_reply() and friends.
 */
#ifndef SPORE_H
#define SPORE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPORE_VERSION "0.1.0"
#define SPORE_MAX_HEADERS 32

typedef struct spore_server spore_server;
typedef struct spore_resp spore_resp;

/* Non-owning byte span. Not NUL-terminated. */
typedef struct {
    const char *ptr;
    size_t len;
} spore_str;

typedef struct {
    spore_str name;
    spore_str value;
} spore_header;

/* A parsed request. Every span points into the connection buffer and is
 * valid only until the handler returns. Copy what an async handler needs. */
typedef struct {
    spore_str method;
    spore_str target;   /* raw request-target, e.g. "/a%20b?x=1" */
    spore_str path;     /* target before '?', not percent-decoded */
    spore_str query;    /* target after '?', empty if absent */
    spore_str tail;     /* path remainder matched by a trailing '*' route */
    int minor;          /* HTTP/1.<minor> */
    spore_header headers[SPORE_MAX_HEADERS];
    size_t n_headers;
    char *body;         /* mutable, NUL-terminated at body[body_len] */
    size_t body_len;
} spore_req;

/* Called on the loop thread. The handler must finish `resp` exactly once,
 * with spore_reply() or spore_end(), now or later from any thread. */
typedef void (*spore_handler)(spore_req *req, spore_resp *resp, void *ud);

/* Zero fields take the default shown. */
typedef struct {
    uint16_t port;              /* loopback TCP port; 0 picks a free one */
    int ipv6;                   /* bind ::1 instead of 127.0.0.1 */
    const char *unix_path;      /* bind this Unix socket instead of TCP */
    const char *token;          /* require "Authorization: Bearer <token>" */
    const char *const *origins; /* NULL-terminated extra allowed Origins */
    size_t max_conns;           /* 64 */
    size_t max_header;          /* 8192 bytes */
    size_t max_body;            /* 8 MiB */
    int idle_ms;                /* keep-alive idle timeout, 30000 */
    int request_ms;             /* deadline to receive a request, 30000 */
} spore_config;

/* Server lifecycle. spore_new() returns NULL and sets errno on failure. */
spore_server *spore_new(const spore_config *cfg);
void spore_free(spore_server *srv);
uint16_t spore_port(const spore_server *srv);

/* Register a route. `method` NULL matches any method; a GET route also
 * serves HEAD. A `pattern` ending in '*' matches by prefix and sets
 * req->tail. First match wins. Returns 0, or -1 on allocation failure. */
int spore_route(spore_server *srv, const char *method, const char *pattern,
                spore_handler fn, void *ud);

/* Run one loop iteration, waiting at most timeout_ms (-1: no limit).
 * Returns 0, or -1 with errno set on a fatal poll error. */
int spore_poll(spore_server *srv, int timeout_ms);

/* Loop until spore_stop(). spore_stop() is safe from any thread or signal
 * handler. */
int spore_run(spore_server *srv);
void spore_stop(spore_server *srv);

/* Request helpers. */
spore_str spore_header_get(const spore_req *req, const char *name);
int spore_str_eq(spore_str s, const char *cstr);  /* exact byte match */
/* Percent-decode into dst ('+' is kept). Returns the decoded length, or -1
 * on a malformed escape, an encoded NUL, or insufficient space. dst is
 * NUL-terminated. */
long spore_url_decode(const char *src, size_t len, char *dst, size_t cap);
/* Find `key` in the query string and decode its value ('+' becomes ' ').
 * Returns the length, or -1 if absent or malformed. */
long spore_query_get(const spore_req *req, const char *key, char *dst,
                     size_t cap);

/* Responses. All functions are thread-safe and return 0 on success, or -1
 * if the client is gone or the call is out of order. After spore_reply() or
 * spore_end() returns, `resp` must not be used again. */
int spore_set_header(spore_resp *resp, const char *name, const char *value);
int spore_reply(spore_resp *resp, int status, const char *ctype,
                const void *body, size_t len);
/* Streaming: chunked on HTTP/1.1, close-delimited on HTTP/1.0. */
int spore_begin(spore_resp *resp, int status, const char *ctype);
int spore_write(spore_resp *resp, const void *data, size_t len);
/* Server-Sent Events: spore_begin() with "text/event-stream" first.
 * `event` may be NULL. Newlines in data become separate "data:" lines. */
int spore_sse(spore_resp *resp, const char *event, const char *data,
              size_t len);
int spore_end(spore_resp *resp);
/* 1 once the client has disconnected; writes will fail. */
int spore_closed(spore_resp *resp);

/* Serve req->tail (or req->path for exact routes) from directory `root`.
 * GET and HEAD only; ".." segments are rejected; symlinks are followed. */
void spore_serve_dir(spore_req *req, spore_resp *resp, const char *root);
const char *spore_mime(const char *path);

/* Growable byte buffer. Allocation failure sets `err` and later appends
 * become no-ops, so callers check once at the end. */
typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
    int err;
} spore_buf;

void spore_buf_add(spore_buf *b, const void *data, size_t len);
void spore_buf_puts(spore_buf *b, const char *s);
void spore_buf_printf(spore_buf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void spore_buf_vprintf(spore_buf *b, const char *fmt, va_list ap);
void spore_buf_free(spore_buf *b);

#ifdef __cplusplus
}
#endif
#endif
