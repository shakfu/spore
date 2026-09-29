/* SPDX-License-Identifier: MIT */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reserve(spore_buf *b, size_t extra) {
    if (b->err) return -1;
    if (extra > (size_t)-1 - b->len - 1) goto fail;
    size_t need = b->len + extra + 1; /* keep room for a NUL */
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap = cap > (size_t)-1 / 2 ? need : cap * 2;
    char *p = realloc(b->ptr, cap);
    if (!p) goto fail;
    b->ptr = p;
    b->cap = cap;
    return 0;
fail:
    b->err = 1;
    return -1;
}

void spore_buf_add(spore_buf *b, const void *data, size_t len) {
    if (reserve(b, len)) return;
    if (len) memcpy(b->ptr + b->len, data, len);
    b->len += len;
    b->ptr[b->len] = '\0';
}

void spore_buf_puts(spore_buf *b, const char *s) {
    spore_buf_add(b, s, strlen(s));
}

void spore_buf_vprintf(spore_buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        b->err = 1;
        return;
    }
    if (reserve(b, (size_t)n)) return;
    vsnprintf(b->ptr + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void spore_buf_printf(spore_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    spore_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void spore_buf_free(spore_buf *b) {
    free(b->ptr);
    memset(b, 0, sizeof *b);
}

/* RFC 4648 section 4, with padding. */
void spore__base64(spore_buf *b, const unsigned char *p, size_t n) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        unsigned long v = (unsigned long)p[i] << 16;
        if (i + 1 < n) v |= (unsigned long)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        char q[4] = {tab[v >> 18 & 63], tab[v >> 12 & 63],
                     i + 1 < n ? tab[v >> 6 & 63] : '=',
                     i + 2 < n ? tab[v & 63] : '='};
        spore_buf_add(b, q, 4);
    }
}
