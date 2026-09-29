/* libFuzzer target: WebSocket frame parser, fed in arbitrary splits. */
#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) abort(); } while (0)
#define MAX_MESSAGE 4096

static int valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t len;
        unsigned long cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
        else return 0;
        if (i + len > n) return 0;
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        static const unsigned long min[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += len;
    }
    return 1;
}

static int closes;

static void on_message(spore_ws *ws, int type, char *data, size_t len,
                       void *ud) {
    (void)ud;
    REQUIRE(len <= MAX_MESSAGE);
    REQUIRE(type == SPORE_WS_TEXT || type == SPORE_WS_BINARY);
    if (type == SPORE_WS_TEXT) REQUIRE(valid_utf8((const unsigned char *)data, len));
    if (len && data[0] == 'q') spore_ws_close(ws, 1000, "q"); /* app-side close */
}

static void on_close(spore_ws *ws, int code, void *ud) {
    (void)ws;
    (void)ud;
    closes++;
    REQUIRE(code >= 1000 && code <= 4999);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    spore_ws_config cfg = {.on_message = on_message, .on_close = on_close,
                           .max_message = MAX_MESSAGE};
    closes = 0;
    spore_ws *ws = spore__ws_detached(&cfg, NULL);
    if (!ws) return 0;
    /* The first byte picks the split size; the rest is the byte stream.
     * Like the connection buffer, unconsumed bytes are kept and extended. */
    size_t step = data[0] % 64 + 1;
    char *buf = malloc(size);
    size_t have = 0;
    for (size_t off = 1; off < size;) {
        size_t n = size - off < step ? size - off : step;
        memcpy(buf + have, data + off, n);
        have += n;
        off += n;
        long used = spore__ws_feed(ws, buf, have);
        REQUIRE(used >= 0 && (size_t)used <= have);
        memmove(buf, buf + used, have - (size_t)used);
        have -= (size_t)used;
    }
    free(buf);
    spore__ws_gone(ws);
    REQUIRE(closes == 1); /* on_close exactly once, whatever happened */
    spore_ws_release(ws);
    return 0;
}
