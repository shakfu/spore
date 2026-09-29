/* JSON reader and writer (RFC 8259).
 * SPDX-License-Identifier: MIT */
#include "spore_json.h"
#include "internal.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct {
    spore_json *doc;
    char *p, *end;
    int depth;
} parser;

static void ws(parser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static long node(parser *ps, spore_jtype t) {
    spore_json *d = ps->doc;
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 32;
        if (cap > UINT32_MAX) return -1;
        spore_jnode *nn = realloc(d->nodes, cap * sizeof *nn);
        if (!nn) return -1;
        d->nodes = nn;
        d->cap = cap;
    }
    memset(&d->nodes[d->n], 0, sizeof d->nodes[0]);
    d->nodes[d->n].type = t;
    return (long)d->n++;
}

static char *put_utf8(char *o, unsigned long cp) {
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | cp >> 6);
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | cp >> 12);
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | cp >> 18);
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

static long hex4(const char *p) {
    long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = (c >= '0' && c <= '9')   ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                       : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

/* Decode in place. Output never outruns input: every escape is at least
 * as long as the UTF-8 it produces. */
static long string(parser *ps) {
    char *src = ++ps->p, *dst = src, *start = src;
    for (;;) {
        if (src >= ps->end) return -1;
        unsigned char c = (unsigned char)*src;
        if (c == '"') break;
        if (c < 0x20) return -1;
        if (c != '\\') {
            *dst++ = *src++;
            continue;
        }
        if (++src >= ps->end) return -1;
        switch (*src++) {
        case '"': *dst++ = '"'; break;
        case '\\': *dst++ = '\\'; break;
        case '/': *dst++ = '/'; break;
        case 'b': *dst++ = '\b'; break;
        case 'f': *dst++ = '\f'; break;
        case 'n': *dst++ = '\n'; break;
        case 'r': *dst++ = '\r'; break;
        case 't': *dst++ = '\t'; break;
        case 'u': {
            long cp = ps->end - src >= 4 ? hex4(src) : -1;
            if (cp < 0) return -1;
            src += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - src >= 6 &&
                src[0] == '\\' && src[1] == 'u') {
                long lo = hex4(src + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    src += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
            dst = put_utf8(dst, (unsigned long)cp);
            break;
        }
        default: return -1;
        }
    }
    long i = node(ps, SPORE_JSTR);
    if (i < 0) return -1;
    *dst = '\0';
    ps->doc->nodes[i].str = start;
    ps->doc->nodes[i].len = (size_t)(dst - start);
    ps->p = src + 1;
    return i;
}

static int digits(parser *ps) {
    char *s = ps->p;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    return ps->p > s;
}

static long number(parser *ps) {
    char *s = ps->p;
    if (*ps->p == '-') ps->p++;
    if (ps->p < ps->end && *ps->p == '0') ps->p++;
    else if (!digits(ps)) return -1;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (!digits(ps)) return -1;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (!digits(ps)) return -1;
    }
    long i = node(ps, SPORE_JNUM);
    if (i < 0) return -1;
    ps->doc->nodes[i].str = s;
    ps->doc->nodes[i].len = (size_t)(ps->p - s);
    return i;
}

static long literal(parser *ps, const char *word, spore_jtype t) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
        return -1;
    ps->p += n;
    return node(ps, t);
}

static long value(parser *ps);

static long container(parser *ps, int obj) {
    long self = node(ps, obj ? SPORE_JOBJ : SPORE_JARR);
    if (self < 0 || ++ps->depth > MAX_DEPTH) return -1;
    char close = obj ? '}' : ']';
    ps->p++;
    ws(ps);
    long last = 0;
    size_t count = 0;
    if (ps->p < ps->end && *ps->p == close) goto done;
    for (;;) {
        for (int k = 0; k <= obj; k++) { /* key then value for objects */
            long child;
            if (obj && k == 0) {
                ws(ps);
                if (ps->p >= ps->end || *ps->p != '"') return -1;
                child = string(ps);
                ws(ps);
                if (child < 0 || ps->p >= ps->end || *ps->p != ':') return -1;
                ps->p++;
            } else {
                child = value(ps);
                if (child < 0) return -1;
            }
            if (last) ps->doc->nodes[last].next = (uint32_t)child;
            else ps->doc->nodes[self].child = (uint32_t)child;
            last = child;
        }
        count++;
        ws(ps);
        if (ps->p >= ps->end) return -1;
        if (*ps->p == close) break;
        if (*ps->p != ',') return -1;
        ps->p++;
    }
done:
    ps->p++;
    ps->depth--;
    ps->doc->nodes[self].len = count;
    return self;
}

static long value(parser *ps) {
    ws(ps);
    if (ps->p >= ps->end) return -1;
    switch (*ps->p) {
    case '{': return container(ps, 1);
    case '[': return container(ps, 0);
    case '"': return string(ps);
    case 't': return literal(ps, "true", SPORE_JTRUE);
    case 'f': return literal(ps, "false", SPORE_JFALSE);
    case 'n': return literal(ps, "null", SPORE_JNULL);
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) return number(ps);
        return -1;
    }
}

int spore_json_parse(spore_json *doc, char *text, size_t len) {
    memset(doc, 0, sizeof *doc);
    parser ps = {doc, text, text + len, 0};
    if (value(&ps) != 0) return -1;
    ws(&ps);
    return ps.p == ps.end ? 0 : -1;
}

void spore_json_free(spore_json *doc) {
    free(doc->nodes);
    memset(doc, 0, sizeof *doc);
}

const spore_jnode *spore_json_root(const spore_json *doc) {
    return doc->n ? &doc->nodes[0] : NULL;
}

const spore_jnode *spore_json_child(const spore_json *doc,
                                    const spore_jnode *n) {
    return n && n->child ? &doc->nodes[n->child] : NULL;
}

const spore_jnode *spore_json_next(const spore_json *doc,
                                   const spore_jnode *n) {
    return n && n->next ? &doc->nodes[n->next] : NULL;
}

const spore_jnode *spore_json_get(const spore_json *doc,
                                  const spore_jnode *obj, const char *key) {
    if (!obj || obj->type != SPORE_JOBJ) return NULL;
    size_t kl = strlen(key);
    for (const spore_jnode *k = spore_json_child(doc, obj); k;) {
        const spore_jnode *v = spore_json_next(doc, k);
        if (k->len == kl && memcmp(k->str, key, kl) == 0) return v;
        k = spore_json_next(doc, v);
    }
    return NULL;
}

/* strtod honours LC_NUMERIC; swap '.' for the locale's radix first. */
int spore_json_double(const spore_jnode *n, double *out) {
    if (!n || n->type != SPORE_JNUM) return -1;
    const char *radix = localeconv()->decimal_point;
    size_t rl = strlen(radix);
    char tmp[128];
    size_t o = 0;
    for (size_t i = 0; i < n->len; i++) {
        if (n->str[i] == '.') {
            if (o + rl >= sizeof tmp) return -1;
            memcpy(tmp + o, radix, rl);
            o += rl;
        } else {
            if (o + 1 >= sizeof tmp) return -1;
            tmp[o++] = n->str[i];
        }
    }
    tmp[o] = '\0';
    *out = strtod(tmp, NULL);
    return 0;
}

int spore_json_int(const spore_jnode *n, long *out) {
    double d;
    if (spore_json_double(n, &d) || d != floor(d) || d < -9.007199254740992e15 ||
        d > 9.007199254740992e15)
        return -1;
    *out = (long)d;
    return 0;
}

int spore_json_bool(const spore_jnode *n, int *out) {
    if (!n || (n->type != SPORE_JTRUE && n->type != SPORE_JFALSE)) return -1;
    *out = n->type == SPORE_JTRUE;
    return 0;
}

/* Length of the valid UTF-8 sequence at s, or 0 if invalid. */
static size_t utf8_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    size_t len;
    unsigned long cp, min;
    if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) len = 3, cp = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) len = 4, cp = c & 0x07, min = 0x10000;
    else return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

int spore__utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        size_t k = utf8_len(p, (size_t)(end - p));
        if (!k) return 0;
        p += k;
    }
    return 1;
}

size_t spore__utf8_cut(const char *s, size_t start, size_t end) {
    for (size_t k = 1; k <= 3 && k <= end - start; k++) {
        unsigned char c = (unsigned char)s[end - k];
        if ((c & 0xC0) == 0x80) continue; /* continuation byte */
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > k ? end - k : end;
    }
    return end;
}

void spore_json_str(spore_buf *b, const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    spore_buf_add(b, "\"", 1);
    while (p < end) {
        const unsigned char *run = p;
        while (p < end && *p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\')
            p++;
        spore_buf_add(b, run, (size_t)(p - run));
        if (p == end) break;
        unsigned char c = *p;
        if (c >= 0x80) {
            size_t n = utf8_len(p, (size_t)(end - p));
            if (n) spore_buf_add(b, p, n), p += n;
            else spore_buf_add(b, "\xEF\xBF\xBD", 3), p++;
            continue;
        }
        const char *esc = c == '"' ? "\\\"" : c == '\\' ? "\\\\"
                        : c == '\n' ? "\\n" : c == '\r' ? "\\r"
                        : c == '\t' ? "\\t" : NULL;
        if (esc) spore_buf_puts(b, esc);
        else spore_buf_printf(b, "\\u%04x", c);
        p++;
    }
    spore_buf_add(b, "\"", 1);
}

void spore_json_num(spore_buf *b, double v) {
    if (isnan(v) || isinf(v)) {
        spore_buf_puts(b, "null");
        return;
    }
    char tmp[40];
    snprintf(tmp, sizeof tmp, "%.15g", v);
    if (strtod(tmp, NULL) != v) snprintf(tmp, sizeof tmp, "%.17g", v);
    /* Undo the locale radix; JSON needs '.'. */
    const char *radix = localeconv()->decimal_point;
    size_t rl = strlen(radix);
    char *r = rl && strcmp(radix, ".") ? strstr(tmp, radix) : NULL;
    if (r) {
        *r = '.';
        memmove(r + 1, r + rl, strlen(r + rl) + 1);
    }
    spore_buf_puts(b, tmp);
}
