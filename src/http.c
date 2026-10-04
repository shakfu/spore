/* HTTP/1.1 request-head parsing (RFC 9112) and request helpers.
 * SPDX-License-Identifier: MIT */
#include "internal.h"

#include <string.h>

static int is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int spore__ieq(spore_str s, const char *cstr) {
    size_t n = strlen(cstr);
    if (s.len != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)s.ptr[i]) != lower((unsigned char)cstr[i]))
            return 0;
    return 1;
}

int spore_str_eq(spore_str s, const char *cstr) {
    size_t n = strlen(cstr);
    return s.len == n && memcmp(s.ptr, cstr, n) == 0;
}

int spore__has_token(spore_str list, const char *tok) {
    size_t i = 0;
    while (i < list.len) {
        while (i < list.len && (list.ptr[i] == ' ' || list.ptr[i] == '\t' ||
                                list.ptr[i] == ','))
            i++;
        size_t start = i;
        while (i < list.len && list.ptr[i] != ',') i++;
        size_t end = i;
        while (end > start && (list.ptr[end - 1] == ' ' || list.ptr[end - 1] == '\t'))
            end--;
        spore_str item = {list.ptr + start, end - start};
        if (item.len && spore__ieq(item, tok)) return 1;
    }
    return 0;
}

spore_str spore_header_get(const spore_req *req, const char *name) {
    for (size_t i = 0; i < req->n_headers; i++)
        if (spore__ieq(req->headers[i].name, name)) return req->headers[i].value;
    return (spore_str){NULL, 0};
}

long spore__parse_head(const char *buf, size_t len, size_t max_header,
                       size_t *scan, spore_req *req) {
    size_t lim = len < max_header ? len : max_header;
    size_t i = *scan > 3 ? *scan - 3 : 0;
    while (i + 4 <= lim && memcmp(buf + i, "\r\n\r\n", 4) != 0) i++;
    if (i + 4 > lim) {
        *scan = lim;
        return len >= max_header ? -431 : 0;
    }
    const char *p = buf, *end = buf + i + 2; /* end: start of final CRLF */

    memset(req, 0, sizeof *req);
    /* request-line = method SP request-target SP HTTP-version CRLF */
    const char *s = p;
    while (p < end && is_tchar((unsigned char)*p)) p++;
    if (p == s || p >= end || *p != ' ') return -400;
    req->method = (spore_str){s, (size_t)(p - s)};
    s = ++p;
    while (p < end && (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7f) p++;
    if (p == s || p >= end || *p != ' ' || *s != '/') return -400;
    req->target = (spore_str){s, (size_t)(p - s)};
    p++;
    if (end - p < 10 || memcmp(p, "HTTP/", 5) != 0 || p[6] != '.' ||
        p[5] < '0' || p[5] > '9' || p[7] < '0' || p[7] > '9' ||
        p[8] != '\r' || p[9] != '\n')
        return -400;
    if (p[5] != '1') return -505;
    req->minor = p[7] - '0';
    p += 10;

    const char *q = memchr(req->target.ptr, '?', req->target.len);
    if (q) {
        req->path = (spore_str){req->target.ptr, (size_t)(q - req->target.ptr)};
        req->query = (spore_str){q + 1, req->target.len - req->path.len - 1};
    } else {
        req->path = req->target;
        req->query = (spore_str){req->target.ptr + req->target.len, 0};
    }

    /* field-line = field-name ":" OWS field-value OWS CRLF */
    while (p < end) {
        if (req->n_headers == SPORE_MAX_HEADERS) return -431;
        s = p;
        while (p < end && is_tchar((unsigned char)*p)) p++;
        if (p == s || p >= end || *p != ':') return -400;
        spore_str name = {s, (size_t)(p - s)};
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        s = p;
        for (; p < end && *p != '\r'; p++) {
            unsigned char c = (unsigned char)*p;
            if (c != '\t' && (c < 0x20 || c == 0x7f)) return -400;
        }
        if (p + 1 > end || p[1] != '\n') return -400;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        req->headers[req->n_headers++] =
            (spore_header){name, {s, (size_t)(e - s)}};
        p += 2;
    }
    return (long)(i + 4);
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

enum { CK_SIZE, CK_DATA, CK_CRLF, CK_TRAILER };

/* No CR, LF or other control byte except HTAB: a bare LF in a chunk line
 * is a known smuggling vector. */
static int line_ok(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return 0;
    }
    return 1;
}

/* chunk-size [ BWS ";" chunk-ext ]; the extension is checked, then ignored */
static int size_line(const char *p, size_t n, uint64_t *size, uint64_t max) {
    size_t i = 0;
    uint64_t v = 0;
    for (; i < n && hexval((unsigned char)p[i]) >= 0; i++) {
        v = v << 4 | (uint64_t)hexval((unsigned char)p[i]);
        if (v > max) return -413;
    }
    if (!i) return -400;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i < n && p[i] != ';') return -400;
    if (!line_ok(p + i, n - i)) return -400;
    *size = v;
    return 0;
}

int spore__dechunk(spore__chunked *st, char *b, size_t *len, size_t max_body,
                   size_t max_line) {
    size_t w = st->out, r = st->out, n = *len;
    int rc = 0;
    while (!rc) {
        if (st->state == CK_DATA) {
            size_t k = n - r < st->left ? n - r : (size_t)st->left;
            if (!k) break;
            memmove(b + w, b + r, k);
            w += k, r += k, st->left -= k;
            if (!st->left) st->state = CK_CRLF;
            continue;
        }
        if (st->state == CK_CRLF) {
            if (n - r < 2) break;
            if (b[r] != '\r' || b[r + 1] != '\n') rc = -400;
            r += 2;
            st->state = CK_SIZE;
            continue;
        }
        const char *nl = memchr(b + r, '\n', n - r);
        if (!nl) {
            if (n - r > max_line) rc = -400;
            break;
        }
        size_t ll = (size_t)(nl - (b + r));
        if (!ll || b[r + ll - 1] != '\r' || ll > max_line) {
            rc = -400;
            break;
        }
        const char *line = b + r;
        ll--; /* without CR */
        r += ll + 2;
        if (st->state == CK_SIZE) {
            rc = size_line(line, ll, &st->left, max_body - w);
            if (!rc) st->state = st->left ? CK_DATA : CK_TRAILER;
        } else if (!ll) {
            rc = 1;
        } else { /* trailer field: checked like a header, then dropped */
            size_t i = 0;
            while (i < ll && is_tchar((unsigned char)line[i])) i++;
            st->trailer += ll + 2;
            if (!i || i == ll || line[i] != ':' || !line_ok(line, ll) ||
                st->trailer > max_line)
                rc = -400;
        }
    }
    memmove(b + w, b + r, n - r);
    *len = w + (n - r);
    st->out = w;
    return rc;
}

static long decode(const char *src, size_t len, char *dst, size_t cap,
                   int plus) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= len) return -1;
            int h = hexval((unsigned char)src[i + 1]);
            int l = hexval((unsigned char)src[i + 2]);
            if (h < 0 || l < 0 || (h | l) == 0) return -1;
            c = (unsigned char)(h << 4 | l);
            i += 2;
        } else if (plus && c == '+') {
            c = ' ';
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = (char)c;
    }
    if (o >= cap) return -1;
    dst[o] = '\0';
    return (long)o;
}

long spore_url_decode(const char *src, size_t len, char *dst, size_t cap) {
    return decode(src, len, dst, cap, 0);
}

long spore_query_get(const spore_req *req, const char *key, char *dst,
                     size_t cap) {
    const char *p = req->query.ptr, *end = p + req->query.len;
    size_t klen = strlen(key);
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *pe = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(pe - p));
        const char *ke = eq ? eq : pe;
        if ((size_t)(ke - p) == klen && memcmp(p, key, klen) == 0) {
            const char *v = eq ? eq + 1 : pe;
            return decode(v, (size_t)(pe - v), dst, cap, 1);
        }
        p = amp ? amp + 1 : end;
    }
    return -1;
}

/* host = "localhost" | "*.localhost" | "127.0.0.1" | "[::1]", optional port */
int spore__host_allowed(spore_str h, const char *const *extra) {
    size_t n = h.len;
    const char *colon = NULL;
    if (n && h.ptr[0] == '[') {
        const char *rb = memchr(h.ptr, ']', n);
        if (!rb) return 0;
        colon = rb + 1 < h.ptr + n ? rb + 1 : NULL;
        if (colon && *colon != ':') return 0;
    } else {
        colon = memchr(h.ptr, ':', n);
    }
    if (colon) {
        for (const char *d = colon + 1; d < h.ptr + n; d++)
            if (*d < '0' || *d > '9') return 0;
        n = (size_t)(colon - h.ptr);
    }
    spore_str name = {h.ptr, n};
    if (spore__ieq(name, "localhost") || spore__ieq(name, "127.0.0.1") ||
        spore__ieq(name, "[::1]"))
        return 1;
    for (; extra && *extra; extra++)
        if (**extra && spore__ieq(name, *extra)) return 1;
    /* RFC 6761: *.localhost always resolves to loopback. */
    static const char sfx[] = ".localhost";
    size_t sl = sizeof sfx - 1;
    return n > sl && spore__ieq((spore_str){name.ptr + n - sl, sl}, sfx);
}

int spore__origin_allowed(spore_str o, const char *const *extra,
                          int loopback) {
    for (; extra && *extra; extra++)
        if (spore_str_eq(o, *extra)) return 1;
    if (!loopback) return 0;
    size_t skip;
    if (o.len > 7 && memcmp(o.ptr, "http://", 7) == 0) skip = 7;
    else if (o.len > 8 && memcmp(o.ptr, "https://", 8) == 0) skip = 8;
    else return 0;
    spore_str host = {o.ptr + skip, o.len - skip};
    if (memchr(host.ptr, '/', host.len)) return 0;
    return spore__host_allowed(host, NULL);
}
