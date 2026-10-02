/* Separate a leading <think> block from generated text.
 * SPDX-License-Identifier: MIT */
#include "internal.h"

#include <string.h>

const char *spore__find(const char *h, size_t hl, const char *n, size_t nl) {
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return h + i;
    return NULL;
}

static int space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

static void out(spore__think *t, const char *a, int think, size_t to,
                spore__think_fn fn, void *ctx) {
    if (to <= t->sent) return;
    fn(ctx, think, a + t->sent, to - t->sent);
    t->sent = to;
}

void spore__think_advance(spore__think *t, const char *a, size_t to, int final,
                          spore__think_fn fn, void *ctx) {
    static const char open[] = "<think>", close[] = "</think>";
    while (t->sent < to) {
        size_t i = t->sent;
        switch (t->mode) {
        case SPORE__THINK_DETECT: {
            while (i < to && space(a[i])) i++;
            size_t m = to - i < 7 ? to - i : 7;
            if (memcmp(a + i, open, m) != 0) {
                t->mode = SPORE__THINK_CONTENT;
            } else if (m == 7) {
                t->sent = i + 7;
                t->mode = SPORE__THINK_IN_TRIM;
            } else if (final) {
                t->mode = SPORE__THINK_CONTENT;
            } else {
                return;
            }
            break;
        }
        case SPORE__THINK_IN_TRIM:
        case SPORE__THINK_OUT_TRIM:
            while (i < to && space(a[i])) i++;
            t->sent = i;
            if (i == to && !final) return;
            t->mode = t->mode == SPORE__THINK_IN_TRIM ? SPORE__THINK_IN
                                                      : SPORE__THINK_CONTENT;
            break;
        case SPORE__THINK_IN: {
            const char *e = spore__find(a + i, to - i, close, 8);
            size_t end = e ? (size_t)(e - a) : to;
            if (!e && !final) /* hold a partial "</think>" */
                for (size_t h = 7; h > 0; h--)
                    if (to - i >= h && memcmp(a + to - h, close, h) == 0) {
                        end = to - h;
                        break;
                    }
            size_t keep = end; /* trailing whitespace waits for more text */
            while (keep > i && space(a[keep - 1])) keep--;
            out(t, a, 1, keep, fn, ctx);
            if (e) {
                t->sent = end + 8;
                t->mode = SPORE__THINK_OUT_TRIM;
            } else {
                if (final) t->sent = to;
                return;
            }
            break;
        }
        default:
            out(t, a, 0, to, fn, ctx);
        }
    }
}
