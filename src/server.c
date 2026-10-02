/* Event loop, connections, responses and the local-only access policy.
 * SPDX-License-Identifier: MIT */
#if defined(__linux__)
#define _GNU_SOURCE /* SO_PEERCRED */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* SO_NOSIGPIPE is set per socket instead */
#endif

#define IN_INITIAL 4096
#define DRAIN_MS 2000

enum { RS_NEW, RS_STREAM, RS_DONE };

struct spore_resp {
    pthread_mutex_t mu;
    spore_server *srv; /* NULL once the loop has let go */
    int refs;          /* loop + handler */
    int state;
    int closed;
    int head_only;
    int minor;
    int keep_alive;
    int chunked;
    size_t max_pending; /* 0: unbounded */
    spore_buf hdrs; /* extra header lines, CRLF-terminated */
    spore_buf out;  /* bytes queued for the socket */
    size_t off;     /* bytes of `out` already sent */
};

typedef struct {
    int fd;
    char *in;
    size_t in_len, in_cap;
    size_t scan;       /* blank-line search offset */
    long head_len;     /* > 0 once the current head passed checks */
    size_t need;       /* head_len + Content-Length */
    const char *base;  /* `in` when the head was parsed */
    spore_req req;
    int draining;      /* write side shut, discarding input until EOF */
    int eof;           /* peer shut its write side: no more input */
    int64_t t_start;   /* first byte of the pending request, 0 if none */
    int64_t t_active;  /* last I/O */
    spore_resp *resp;
    spore__upgrade_hooks up; /* set once upgraded: input goes to up.on_data */
    size_t up_max;           /* input buffer bound after the upgrade */
} conn;

typedef struct {
    char *method;
    char *pattern;
    size_t plen;
    int prefix;
    spore_handler fn;
    void *ud;
} route;

struct spore_server {
    spore_config cfg;
    char *token;
    char *unix_path;
    int lfd;
    int wake[2];
    atomic_int wake_pending;
    atomic_int stop;
    uint16_t port;
    route *routes;
    size_t n_routes;
    conn **conns;
    size_t n_conns;
    struct pollfd *pfds;
    conn *dispatching; /* connection whose handler is running */
};

/* Set while a thread runs spore_poll(); lets responses skip the wake-up
 * write when completed on the loop thread itself. */
static _Thread_local spore_server *tl_loop;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_flags(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void wake(spore_server *s) {
    if (tl_loop == s) return;
    if (!atomic_exchange(&s->wake_pending, 1)) {
        ssize_t n = write(s->wake[1], "", 1);
        (void)n; /* a full pipe already guarantees a wake-up */
    }
}

/* ---- responses -------------------------------------------------------- */

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

/* IMF-fixdate (RFC 9110 5.6.7); strftime's %a and %b follow the locale. */
static void http_date(char out[32]) {
    static const char *const wd[] = {"Sun", "Mon", "Tue", "Wed",
                                     "Thu", "Fri", "Sat"};
    static const char *const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, 32, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[tm.tm_wday],
             tm.tm_mday, mo[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
}

/* clen < 0 starts a streamed body. Caller holds r->mu. */
static void write_head(spore_resp *r, int status, const char *ctype,
                       long clen) {
    char date[32];
    http_date(date);
    spore_buf *o = &r->out;
    spore_buf_printf(o, "HTTP/1.1 %d %s\r\nDate: %s\r\n", status,
                     reason(status), date);
    if (ctype) spore_buf_printf(o, "Content-Type: %s\r\n", ctype);
    if (status == 204 || status == 304) {
        /* no body, no length */
    } else if (clen >= 0) {
        spore_buf_printf(o, "Content-Length: %ld\r\n", clen);
    } else if (r->minor >= 1) {
        spore_buf_puts(o, "Transfer-Encoding: chunked\r\n");
        r->chunked = 1;
    } else {
        r->keep_alive = 0;
    }
    if (!r->keep_alive) spore_buf_puts(o, "Connection: close\r\n");
    spore_buf_add(o, r->hdrs.ptr, r->hdrs.len);
    spore_buf_puts(o, "\r\n");
}

static void release(spore_resp *r) {
    pthread_mutex_lock(&r->mu);
    int n = --r->refs;
    pthread_mutex_unlock(&r->mu);
    if (n) return;
    pthread_mutex_destroy(&r->mu);
    spore_buf_free(&r->hdrs);
    spore_buf_free(&r->out);
    free(r);
}

void spore__resp_retain(spore_resp *r) {
    pthread_mutex_lock(&r->mu);
    r->refs++;
    pthread_mutex_unlock(&r->mu);
}

void spore__resp_release(spore_resp *r) { release(r); }

/* Caller holds r->mu; drops the handler's reference. */
static void finish(spore_resp *r) {
    if (r->state == RS_STREAM && r->chunked && !r->head_only && !r->closed)
        spore_buf_puts(&r->out, "0\r\n\r\n");
    r->state = RS_DONE;
    if (r->srv) wake(r->srv);
    pthread_mutex_unlock(&r->mu);
    release(r);
}

static int valid_header(const char *s, int name) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r' || c == '\n' || c == 0x7f || (c < 0x20 && c != '\t'))
            return 0;
        if (name && (c == ':' || c <= ' ')) return 0;
    }
    return 1;
}

int spore_set_header(spore_resp *r, const char *name, const char *value) {
    if (!*name || !valid_header(name, 1) || !valid_header(value, 0))
        return -1;
    pthread_mutex_lock(&r->mu);
    int ok = r->state == RS_NEW && !r->closed;
    if (ok) spore_buf_printf(&r->hdrs, "%s: %s\r\n", name, value);
    pthread_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

int spore_reply(spore_resp *r, int status, const char *ctype,
                const void *body, size_t len) {
    pthread_mutex_lock(&r->mu);
    if (r->state == RS_DONE) {
        pthread_mutex_unlock(&r->mu);
        return -1;
    }
    int ok = r->state == RS_NEW && !r->closed;
    if (ok) {
        write_head(r, status, ctype, (long)len);
        if (!r->head_only && status != 204 && status != 304)
            spore_buf_add(&r->out, body, len);
        ok = !r->out.err;
    }
    finish(r);
    return ok ? 0 : -1;
}

int spore_begin(spore_resp *r, int status, const char *ctype) {
    pthread_mutex_lock(&r->mu);
    int ok = r->state == RS_NEW && !r->closed;
    if (ok) {
        write_head(r, status, ctype, -1);
        r->state = RS_STREAM;
        if (r->srv) wake(r->srv);
    }
    pthread_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

/* Caller holds r->mu. A client that stops reading must not make `out`
 * grow without bound: past the cap, treat it as gone. */
static int overflow(spore_resp *r, size_t add) {
    if (!r->max_pending || r->out.len - r->off + add <= r->max_pending)
        return 0;
    r->closed = 1;
    if (r->srv) wake(r->srv);
    return 1;
}

int spore_write(spore_resp *r, const void *data, size_t len) {
    pthread_mutex_lock(&r->mu);
    int ok = r->state == RS_STREAM && !r->closed && !r->out.err &&
             !overflow(r, r->head_only ? 0 : len);
    if (ok && len && !r->head_only) {
        if (r->chunked) spore_buf_printf(&r->out, "%zx\r\n", len);
        spore_buf_add(&r->out, data, len);
        if (r->chunked) spore_buf_puts(&r->out, "\r\n");
        if (r->srv) wake(r->srv);
    }
    pthread_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

int spore_sse(spore_resp *r, const char *event, const char *data,
              size_t len) {
    if (event && (strchr(event, '\n') || strchr(event, '\r'))) return -1;
    spore_buf b = {0};
    if (event) spore_buf_printf(&b, "event: %s\n", event);
    for (size_t i = 0;;) {
        size_t j = i;
        while (j < len && data[j] != '\n' && data[j] != '\r') j++;
        spore_buf_puts(&b, "data: ");
        spore_buf_add(&b, data + i, j - i);
        spore_buf_puts(&b, "\n");
        if (j >= len) break;
        if (data[j] == '\r' && j + 1 < len && data[j + 1] == '\n') j++;
        i = j + 1;
    }
    spore_buf_puts(&b, "\n");
    int rc = b.err ? -1 : spore_write(r, b.ptr, b.len);
    spore_buf_free(&b);
    return rc;
}

int spore_end(spore_resp *r) {
    pthread_mutex_lock(&r->mu);
    if (r->state == RS_DONE) {
        pthread_mutex_unlock(&r->mu);
        return -1;
    }
    int ok = r->state == RS_STREAM && !r->closed;
    if (r->state == RS_NEW && !r->closed) /* never begun: do not hang */
        write_head(r, 500, NULL, 0);
    finish(r);
    return ok ? 0 : -1;
}

int spore__write2(spore_resp *r, const void *a, size_t alen, const void *b,
                  size_t blen, int end) {
    pthread_mutex_lock(&r->mu);
    int ok = r->state == RS_STREAM && !r->closed && !r->out.err &&
             !overflow(r, r->head_only ? 0 : alen + blen);
    if (ok && !r->head_only) {
        if (r->chunked) spore_buf_printf(&r->out, "%zx\r\n", alen + blen);
        spore_buf_add(&r->out, a, alen);
        spore_buf_add(&r->out, b, blen);
        if (r->chunked) spore_buf_puts(&r->out, "\r\n");
        if (r->srv) wake(r->srv);
    }
    if (end && r->state != RS_DONE) {
        finish(r); /* unlocks and drops the handler's reference */
        return ok ? 0 : -1;
    }
    pthread_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

size_t spore__pending(spore_resp *r) {
    pthread_mutex_lock(&r->mu);
    size_t n = r->out.len - r->off;
    pthread_mutex_unlock(&r->mu);
    return n;
}

void spore__resp_take(spore_resp *r, spore_buf *dst) {
    pthread_mutex_lock(&r->mu);
    if (r->out.len > r->off)
        spore_buf_add(dst, r->out.ptr + r->off, r->out.len - r->off);
    r->off = r->out.len = 0;
    pthread_mutex_unlock(&r->mu);
}

int spore_closed(spore_resp *r) {
    pthread_mutex_lock(&r->mu);
    int c = r->closed;
    pthread_mutex_unlock(&r->mu);
    return c;
}

static spore_resp *resp_new(spore_server *s, const spore_req *req, int keep) {
    spore_resp *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    pthread_mutex_init(&r->mu, NULL);
    r->srv = s;
    r->refs = 2;
    r->minor = req ? req->minor : 1;
    r->head_only = req && spore_str_eq(req->method, "HEAD");
    r->keep_alive = keep;
    r->max_pending = s ? s->cfg.max_pending : 0;
    if (req) {
        spore_str o = spore_header_get(req, "Origin");
        if (o.ptr && spore__origin_allowed(o, s->cfg.origins,
                                           !s->cfg.origins_only))
            spore_buf_printf(&r->hdrs,
                             "Access-Control-Allow-Origin: %.*s\r\n"
                             "Vary: Origin\r\n",
                             (int)o.len, o.ptr);
    }
    return r;
}

spore_resp *spore__resp_detached(void) {
    spore_resp *r = resp_new(NULL, NULL, 0);
    if (r) r->state = RS_STREAM;
    return r;
}

int spore__upgrade(spore_resp *r, const char *extra,
                   const spore__upgrade_hooks *hooks, size_t max_in) {
    spore_server *s = r->srv; /* written only by the loop thread */
    if (!s || tl_loop != s || !s->dispatching || s->dispatching->resp != r)
        return -1;
    pthread_mutex_lock(&r->mu);
    int ok = r->state == RS_NEW && !r->closed;
    if (ok) {
        spore_buf_puts(&r->out, "HTTP/1.1 101 Switching Protocols\r\n");
        spore_buf_add(&r->out, r->hdrs.ptr, r->hdrs.len);
        if (extra) spore_buf_puts(&r->out, extra);
        spore_buf_puts(&r->out, "\r\n");
        r->state = RS_STREAM;
        r->chunked = 0;
        r->keep_alive = 0;
        r->head_only = 0;
    }
    pthread_mutex_unlock(&r->mu);
    if (!ok) return -1;
    s->dispatching->up = *hooks;
    s->dispatching->up_max = max_in;
    return 0;
}

/* ---- connections ------------------------------------------------------ */

/* The loop gives up its reference; the handler may still hold one. */
static void detach(conn *c, int closed) {
    spore_resp *r = c->resp;
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->srv = NULL;
    if (closed) r->closed = 1;
    pthread_mutex_unlock(&r->mu);
    release(r);
    c->resp = NULL;
}

static void conn_free(conn *c) {
    detach(c, 1);
    if (c->up.on_close) c->up.on_close(c->up.ctx);
    close(c->fd);
    free(c->in);
    free(c);
}

static void conn_close(spore_server *s, size_t i) {
    conn_free(s->conns[i]);
    s->conns[i] = s->conns[--s->n_conns];
}

static void start_drain(conn *c, int64_t now) {
    shutdown(c->fd, SHUT_WR);
    c->draining = 1;
    c->in_len = 0;
    c->t_active = now;
}

/* Loop-generated reply. keep == 0 closes the connection afterwards. */
static int simple_reply(spore_server *s, conn *c, const spore_req *req,
                        int status, int keep, const char *extra) {
    spore_resp *r = resp_new(s, req, keep);
    if (!r) return -1;
    c->resp = r;
    if (extra) spore_buf_puts(&r->hdrs, extra);
    const char *msg = reason(status);
    char body[64];
    int n = snprintf(body, sizeof body, "%d %s\n", status, msg);
    return spore_reply(r, status, "text/plain", body, (size_t)n);
}

static int ct_equal(const char *a, size_t alen, const char *b, size_t blen) {
    unsigned char d = alen != blen;
    size_t n = alen < blen ? alen : blen;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* Browsers cannot set Authorization on a WebSocket; OpenAI's convention
 * carries the key as the subprotocol "openai-insecure-api-key.<key>". */
static int protocol_token(spore_str list, const char *tok, size_t tl) {
    static const char pfx[] = "openai-insecure-api-key.";
    size_t pl = sizeof pfx - 1;
    const char *p = list.ptr, *end = list.ptr + list.len;
    while (p && p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        const char *e = p;
        while (e < end && *e != ',' && *e != ' ' && *e != '\t') e++;
        if ((size_t)(e - p) > pl && memcmp(p, pfx, pl) == 0 &&
            ct_equal(p + pl, (size_t)(e - p) - pl, tok, tl))
            return 1;
        p = e;
    }
    return 0;
}

/* Access policy and framing. Returns 0 to dispatch, 1 if a reply was
 * queued, or an error status to send before closing. */
static int check(spore_server *s, conn *c, spore_req *req, size_t *clen) {
    int check_host = !s->unix_path || s->cfg.hosts;
    spore_str host = {0}, origin = {0}, cl = {0};
    int n_host = 0, n_cl = 0;
    for (size_t i = 0; i < req->n_headers; i++) {
        spore_header *h = &req->headers[i];
        if (spore__ieq(h->name, "Host")) host = h->value, n_host++;
        else if (spore__ieq(h->name, "Origin")) origin = h->value;
        else if (spore__ieq(h->name, "Content-Length")) cl = h->value, n_cl++;
        else if (spore__ieq(h->name, "Transfer-Encoding")) return 501;
    }
    if (n_host > 1 || (req->minor >= 1 && n_host == 0)) return 400;
    /* DNS rebinding: a hostile page reaches loopback under its own name. */
    if (check_host && n_host && !spore__host_allowed(host, s->cfg.hosts))
        return 403;
    if (origin.ptr && !spore__origin_allowed(origin, s->cfg.origins,
                                             !s->cfg.origins_only))
        return 403;

    if (n_cl > 1) return 400;
    size_t n = 0;
    for (size_t i = 0; i < cl.len; i++) {
        if (cl.ptr[i] < '0' || cl.ptr[i] > '9') return 400;
        if (n > s->cfg.max_body) break;
        n = n * 10 + (size_t)(cl.ptr[i] - '0');
    }
    if (n_cl && !cl.len) return 400;
    if (n > s->cfg.max_body) return 413;
    *clen = n;

    int keep = req->minor >= 1 &&
               !spore__has_token(spore_header_get(req, "Connection"), "close");

    spore_str acrm = spore_header_get(req, "Access-Control-Request-Method");
    if (origin.ptr && acrm.ptr && spore_str_eq(req->method, "OPTIONS")) {
        spore_buf h = {0};
        spore_str acrh = spore_header_get(req, "Access-Control-Request-Headers");
        spore_buf_printf(&h,
                         "Access-Control-Allow-Methods: %.*s\r\n"
                         "Access-Control-Max-Age: 600\r\n",
                         (int)acrm.len, acrm.ptr);
        if (acrh.len)
            spore_buf_printf(&h, "Access-Control-Allow-Headers: %.*s\r\n",
                             (int)acrh.len, acrh.ptr);
        int rc = h.err ? -1 : simple_reply(s, c, req, 204, keep && !n, h.ptr);
        spore_buf_free(&h);
        return rc < 0 ? 500 : 1;
    }

    if (s->token) {
        spore_str a = spore_header_get(req, "Authorization");
        size_t tl = strlen(s->token);
        int ok = a.len >= 7 && spore__ieq((spore_str){a.ptr, 7}, "Bearer ") &&
                 ct_equal(a.ptr + 7, a.len - 7, s->token, tl);
        if (!ok)
            ok = protocol_token(spore_header_get(req, "Sec-WebSocket-Protocol"),
                                s->token, tl);
        if (!ok) {
            if (simple_reply(s, c, req, 401, keep && !n,
                             "WWW-Authenticate: Bearer\r\n") < 0)
                return 500;
            return 1;
        }
    }
    return 0;
}

static void dispatch(spore_server *s, conn *c, spore_req *req) {
    int keep = req->minor >= 1 &&
               !spore__has_token(spore_header_get(req, "Connection"), "close");
    int path_hit = 0;
    for (size_t i = 0; i < s->n_routes; i++) {
        route *rt = &s->routes[i];
        if (rt->prefix) {
            if (req->path.len < rt->plen ||
                memcmp(req->path.ptr, rt->pattern, rt->plen) != 0)
                continue;
        } else if (!spore_str_eq(req->path, rt->pattern)) {
            continue;
        }
        path_hit = 1;
        if (rt->method && !spore_str_eq(req->method, rt->method) &&
            !(strcmp(rt->method, "GET") == 0 &&
              spore_str_eq(req->method, "HEAD")))
            continue;
        req->tail = rt->prefix ? (spore_str){req->path.ptr + rt->plen,
                                             req->path.len - rt->plen}
                               : (spore_str){req->path.ptr + req->path.len, 0};
        spore_resp *r = resp_new(s, req, keep);
        if (!r) {
            simple_reply(s, c, NULL, 500, 0, NULL);
            return;
        }
        c->resp = r;
        s->dispatching = c;
        rt->fn(req, r, rt->ud);
        s->dispatching = NULL;
        return;
    }
    simple_reply(s, c, req, path_hit ? 405 : 404, keep, NULL);
}

/* Returns 1 when the response is complete and flushed, 0 if pending,
 * -1 on a socket error. */
static int flush(conn *c, int64_t now) {
    spore_resp *r = c->resp;
    int rc = 0;
    pthread_mutex_lock(&r->mu);
    while (r->off < r->out.len) {
        ssize_t n = send(c->fd, r->out.ptr + r->off, r->out.len - r->off,
                         SEND_FLAGS);
        if (n > 0) {
            r->off += (size_t)n;
            c->t_active = now;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            rc = -1;
            break;
        }
    }
    if (r->off == r->out.len) {
        r->off = r->out.len = 0;
    } else if (r->off > 65536 && r->off > r->out.len / 2) {
        memmove(r->out.ptr, r->out.ptr + r->off, r->out.len - r->off);
        r->out.len -= r->off;
        r->off = 0;
    }
    if (r->out.err || r->closed) rc = -1; /* closed: overflow() */
    if (!rc && r->state == RS_DONE && !r->out.len) rc = 1;
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static int want_write(conn *c) {
    if (!c->resp) return 0;
    pthread_mutex_lock(&c->resp->mu);
    int w = c->resp->out.len > c->resp->off;
    pthread_mutex_unlock(&c->resp->mu);
    return w;
}

static size_t in_limit(spore_server *s, conn *c) {
    if (c->draining) return IN_INITIAL;
    if (c->up.on_data) return c->up_max;
    return c->head_len ? c->need + 1 : s->cfg.max_header + 1;
}

static int grow_in(conn *c, size_t cap) {
    if (cap <= c->in_cap) return 0;
    char *p = realloc(c->in, cap);
    if (!p) return -1;
    c->in = p;
    c->in_cap = cap;
    return 0;
}

/* Returns -1 when the connection should close. */
static int conn_read(spore_server *s, conn *c, int64_t now) {
    for (;;) {
        if (c->draining) c->in_len = 0;
        if (c->in_len == c->in_cap) {
            size_t lim = in_limit(s, c);
            if (c->in_cap >= lim) return 0; /* full; wait for dispatch */
            size_t cap = c->in_cap * 2 < lim ? c->in_cap * 2 : lim;
            if (grow_in(c, cap)) return -1;
        }
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n > 0) {
            if (!c->in_len && !c->t_start && !c->resp) c->t_start = now;
            c->in_len += (size_t)n;
            c->t_active = now;
            continue;
        }
        if (n == 0) { /* half-close: answer what was sent, then close */
            if (c->up.on_data) return -1;
            c->eof = 1;
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

/* Upgraded connection: flush, hand input to the hook, flush its replies. */
static int serve_upgraded(conn *c, int64_t now) {
    for (int pass = 0; pass < 2; pass++) {
        int rc = flush(c, now);
        if (rc < 0) return -1;
        if (rc == 1) { /* protocol finished: close after the last byte */
            detach(c, 0);
            start_drain(c, now);
            return 0;
        }
        if (pass || !c->in_len) return 0;
        long n = c->up.on_data(c->up.ctx, c->in, c->in_len);
        if (n < 0) return -1;
        memmove(c->in, c->in + n, c->in_len - (size_t)n);
        c->in_len -= (size_t)n;
    }
    return 0;
}

static int conn_service(spore_server *s, conn *c, int64_t now) {
    for (;;) {
        if (c->up.on_data && c->resp) {
            int rc = serve_upgraded(c, now);
            return c->eof ? -1 : rc;
        }
        if (c->resp) {
            int rc = flush(c, now);
            if (rc < 0) return -1;
            if (rc == 0) return 0;
            int keep = c->resp->keep_alive;
            detach(c, 0);
            if (!keep) {
                start_drain(c, now);
                return 0;
            }
            c->t_start = c->in_len ? now : 0;
            if (c->in_cap > 65536 && c->in_len < IN_INITIAL) {
                char *p = realloc(c->in, IN_INITIAL);
                if (p) c->in = p, c->in_cap = IN_INITIAL;
            }
            continue;
        }
        if (c->draining)
            return c->eof || now - c->t_active > DRAIN_MS ? -1 : 0;

        spore_req *req = &c->req;
        if (!c->head_len) {
            if (!c->in_len) break;
            long h = spore__parse_head(c->in, c->in_len, s->cfg.max_header,
                                       &c->scan, req);
            if (h == 0) break;
            size_t clen = 0;
            int st = h < 0 ? (int)-h : check(s, c, req, &clen);
            if (st == 1) { /* reply queued; a body forces close + drain */
                size_t used = clen ? c->in_len : (size_t)h;
                memmove(c->in, c->in + used, c->in_len - used);
                c->in_len -= used;
                c->scan = 0;
                continue;
            }
            if (st) {
                if (simple_reply(s, c, h < 0 ? NULL : req, st, 0, NULL) < 0)
                    return -1;
                continue;
            }
            c->head_len = h;
            c->need = (size_t)h + clen;
            /* Before grow_in(): its realloc leaves `req` pointing at freed memory. */
            int expect = spore__ieq(spore_header_get(req, "Expect"), "100-continue");
            c->base = c->in;
            if (grow_in(c, c->need + 1)) return -1;
            if (c->in_len < c->need && expect) {
                static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (send(c->fd, cont, sizeof cont - 1, SEND_FLAGS) !=
                    (ssize_t)(sizeof cont - 1))
                    return -1;
            }
        }
        if (c->in_len < c->need) break;

        if (c->base != c->in) { /* buffer moved: re-point the spans */
            size_t scan = 0;
            spore__parse_head(c->in, c->in_len, s->cfg.max_header, &scan, req);
        }
        req->body = c->in + c->head_len;
        req->body_len = c->need - (size_t)c->head_len;
        char saved = c->in[c->need];
        c->in[c->need] = '\0';
        dispatch(s, c, req);
        c->in[c->need] = saved;

        memmove(c->in, c->in + c->need, c->in_len - c->need);
        c->in_len -= c->need;
        c->head_len = 0;
        c->need = 0;
        c->scan = 0;
    }
    if (!c->resp && !c->draining) {
        if (c->eof) return -1; /* no further request can arrive */
        if (c->t_start && now - c->t_start > s->cfg.request_ms) {
            if (simple_reply(s, c, NULL, 408, 0, NULL) < 0) return -1;
            return conn_service(s, c, now);
        }
        if (!c->t_start && now - c->t_active > s->cfg.idle_ms) return -1;
    }
    return 0;
}

static int peer_allowed(spore_server *s, int fd,
                        const struct sockaddr_storage *ss) {
    if (s->unix_path) {
#if defined(SO_PEERCRED)
        struct ucred cr;
        socklen_t len = sizeof cr;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0) return 0;
        return cr.uid == geteuid();
#else
        uid_t uid;
        gid_t gid;
        if (getpeereid(fd, &uid, &gid) < 0) return 0;
        return uid == geteuid();
#endif
    }
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        return (ntohl(a->sin_addr.s_addr) >> 24) == 127;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        return memcmp(&a->sin6_addr, &in6addr_loopback, 16) == 0;
    }
    return 0;
}

/* Register an accepted socket. Returns 0, or -1 (the caller closes fd). */
static int add_conn(spore_server *s, int fd, int64_t now) {
    int one = 1;
    if (set_flags(fd) < 0) return -1;
    if (!s->unix_path)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    conn *c = calloc(1, sizeof *c);
    size_t cap = s->cfg.max_header + 1 < IN_INITIAL ? s->cfg.max_header + 1
                                                     : IN_INITIAL;
    if (!c || !(c->in = malloc(cap))) {
        free(c);
        return -1;
    }
    c->in_cap = cap;
    c->fd = fd;
    c->t_active = now;
    s->conns[s->n_conns++] = c;
    return 0;
}

static void accept_all(spore_server *s, int64_t now) {
    while (s->n_conns < s->cfg.max_conns) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept(s->lfd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return; /* EAGAIN, or EMFILE and friends: retry next poll */
        }
        if (!peer_allowed(s, fd, &ss) || add_conn(s, fd, now) < 0) close(fd);
    }
}

int spore__adopt(spore_server *s, int fd) {
    if (s->n_conns >= s->cfg.max_conns) return -1;
    return add_conn(s, fd, now_ms());
}

/* When conn_service() would next act on a timeout, or INT64_MAX. */
static int64_t deadline(spore_server *s, conn *c) {
    if (c->draining) return c->t_active + DRAIN_MS + 1;
    if (c->resp) return INT64_MAX;
    if (c->t_start) return c->t_start + s->cfg.request_ms + 1;
    return c->t_active + s->cfg.idle_ms + 1;
}

int spore_poll(spore_server *s, int timeout_ms) {
    tl_loop = s;
    size_t n = 2;
    int64_t next = INT64_MAX;
    s->pfds[0] = (struct pollfd){s->wake[0], POLLIN, 0};
    s->pfds[1] = (struct pollfd){s->n_conns < s->cfg.max_conns ? s->lfd : -1,
                                 POLLIN, 0};
    for (size_t i = 0; i < s->n_conns; i++) {
        conn *c = s->conns[i];
        short ev = 0;
        if (!c->eof && (c->in_len < c->in_cap || c->in_cap < in_limit(s, c)))
            ev |= POLLIN;
        if (want_write(c)) ev |= POLLOUT;
        s->pfds[n++] = (struct pollfd){c->fd, ev, 0};
        int64_t d = deadline(s, c);
        if (d < next) next = d;
    }
    if (next != INT64_MAX) {
        int64_t wait = next - now_ms();
        if (wait < 0) wait = 0;
        if (timeout_ms < 0 || wait < timeout_ms) timeout_ms = (int)wait;
    }
    if (poll(s->pfds, n, timeout_ms) < 0 && errno != EINTR) {
        tl_loop = NULL;
        return -1;
    }
    int64_t now = now_ms();
    if (s->pfds[0].revents & POLLIN) {
        /* Drain before clearing: the reverse order can swallow a byte
         * written after the clear, leaving wake_pending stuck at 1. */
        char tmp[64];
        while (read(s->wake[0], tmp, sizeof tmp) > 0) {}
        atomic_store(&s->wake_pending, 0);
    }
    /* Reverse order: conn_close() moves the last connection into slot i. */
    for (size_t i = s->n_conns; i-- > 0;) {
        conn *c = s->conns[i];
        short re = s->pfds[2 + i].revents;
        /* POLLHUP: the peer cannot receive (POSIX), so stop even with input
         * unread, as when the buffer is full. A draining connection still
         * reads to EOF: closing with unread input sends RST, which can
         * destroy the reply the peer has not yet read. */
        if ((re & POLLERR) || ((re & POLLHUP) && !c->draining) ||
            ((re & (POLLIN | POLLHUP)) && conn_read(s, c, now) < 0)) {
            conn_close(s, i);
            continue;
        }
        if (conn_service(s, c, now) < 0) conn_close(s, i);
    }
    if (s->pfds[1].revents & POLLIN) accept_all(s, now);
    tl_loop = NULL;
    return 0;
}

int spore_run(spore_server *s) {
    while (!atomic_load(&s->stop))
        if (spore_poll(s, -1) < 0) return -1;
    atomic_store(&s->stop, 0);
    return 0;
}

void spore_stop(spore_server *s) {
    atomic_store(&s->stop, 1);
    ssize_t n = write(s->wake[1], "", 1);
    (void)n;
}

/* ---- setup ------------------------------------------------------------ */

int spore_route(spore_server *s, const char *method, const char *pattern,
                spore_handler fn, void *ud) {
    route *rs = realloc(s->routes, (s->n_routes + 1) * sizeof *rs);
    if (!rs) return -1;
    s->routes = rs;
    route *r = &rs[s->n_routes];
    memset(r, 0, sizeof *r);
    size_t plen = strlen(pattern);
    r->prefix = plen && pattern[plen - 1] == '*';
    r->plen = plen - (size_t)r->prefix;
    r->pattern = strndup(pattern, r->plen);
    r->method = method ? strdup(method) : NULL;
    if (!r->pattern || (method && !r->method)) {
        free(r->pattern);
        free(r->method);
        return -1;
    }
    r->fn = fn;
    r->ud = ud;
    s->n_routes++;
    return 0;
}

void spore__unroute(spore_server *s, void *ud) {
    size_t n = 0;
    for (size_t i = 0; i < s->n_routes; i++) {
        if (s->routes[i].ud == ud) {
            free(s->routes[i].method);
            free(s->routes[i].pattern);
        } else {
            s->routes[n++] = s->routes[i];
        }
    }
    s->n_routes = n;
}

static int listen_tcp(spore_server *s) {
    int fam = s->cfg.ipv6 ? AF_INET6 : AF_INET;
    int fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (fam == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        a->sin6_port = htons(s->cfg.port);
        sl = sizeof *a;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a->sin_port = htons(s->cfg.port);
        sl = sizeof *a;
    }
    if (bind(fd, (struct sockaddr *)&ss, sl) < 0 ||
        getsockname(fd, (struct sockaddr *)&ss, &sl) < 0)
        goto fail;
    s->port = ntohs(fam == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port
                                    : ((struct sockaddr_in *)&ss)->sin_port);
    return fd;
fail:;
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

static int listen_unix(spore_server *s) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(s->unix_path) >= sizeof a.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(a.sun_path, s->unix_path);
    struct stat st;
    if (lstat(s->unix_path, &st) == 0 && S_ISSOCK(st.st_mode))
        unlink(s->unix_path); /* stale socket from an earlier run */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        chmod(s->unix_path, 0600) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

spore_server *spore_new(const spore_config *cfg) {
    spore_server *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->lfd = s->wake[0] = s->wake[1] = -1;
    if (cfg) s->cfg = *cfg;
    spore_config *c = &s->cfg;
    if (!c->max_conns) c->max_conns = 64;
    if (!c->max_header) c->max_header = 8192;
    if (!c->max_body) c->max_body = 8u << 20;
    if (!c->idle_ms) c->idle_ms = 30000;
    if (!c->request_ms) c->request_ms = 30000;
    if (!c->max_pending) c->max_pending = 16u << 20;
    if ((c->token && !(s->token = strdup(c->token))) ||
        (c->unix_path && !(s->unix_path = strdup(c->unix_path))))
        goto fail;
    s->conns = calloc(c->max_conns, sizeof *s->conns);
    s->pfds = calloc(c->max_conns + 2, sizeof *s->pfds);
    if (!s->conns || !s->pfds || pipe(s->wake) < 0) goto fail;
    if (set_flags(s->wake[0]) < 0 || set_flags(s->wake[1]) < 0) goto fail;
    s->lfd = s->unix_path ? listen_unix(s) : listen_tcp(s);
    if (s->lfd < 0 || listen(s->lfd, 128) < 0 || set_flags(s->lfd) < 0)
        goto fail;
    return s;
fail:;
    int e = errno;
    spore_free(s);
    errno = e;
    return NULL;
}

void spore_free(spore_server *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n_conns; i++) conn_free(s->conns[i]);
    if (s->lfd >= 0) {
        close(s->lfd);
        if (s->unix_path) unlink(s->unix_path);
    }
    if (s->wake[0] >= 0) close(s->wake[0]);
    if (s->wake[1] >= 0) close(s->wake[1]);
    for (size_t i = 0; i < s->n_routes; i++) {
        free(s->routes[i].method);
        free(s->routes[i].pattern);
    }
    free(s->routes);
    free(s->conns);
    free(s->pfds);
    free(s->token);
    free(s->unix_path);
    free(s);
}

uint16_t spore_port(const spore_server *s) { return s->port; }
