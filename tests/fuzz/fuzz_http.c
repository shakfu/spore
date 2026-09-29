/* libFuzzer target: HTTP request-head parser and request helpers. */
#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) abort(); } while (0)

static int inside(spore_str s, const char *lo, const char *hi) {
    return !s.len || (s.ptr >= lo && s.ptr + s.len <= hi);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *buf = (const char *)data;
    const size_t max = 8192;
    spore_req r;
    size_t scan = 0;
    long h = spore__parse_head(buf, size, max, &scan, &r);
    REQUIRE(h <= (long)size);

    /* Feeding the bytes in steps, with `scan` carried over, must agree. */
    size_t step = size % 13 + 1, scan2 = 0;
    long h2 = 0;
    spore_req r2;
    for (size_t n = step < size ? step : size; h2 == 0; n += step) {
        if (n > size) n = size;
        h2 = spore__parse_head(buf, n, max, &scan2, &r2);
        if (n == size) break;
    }
    REQUIRE(h2 == h);

    if (h <= 0) return 0;
    const char *lo = buf, *hi = buf + h;
    REQUIRE(r.method.len > 0 && inside(r.method, lo, hi));
    REQUIRE(r.target.len > 0 && r.target.ptr[0] == '/');
    REQUIRE(inside(r.target, lo, hi) && inside(r.path, lo, hi) &&
            inside(r.query, lo, hi));
    REQUIRE(r.path.len <= r.target.len && r.minor >= 0 && r.minor <= 9);
    REQUIRE(r.n_headers <= SPORE_MAX_HEADERS);
    for (size_t i = 0; i < r.n_headers; i++) {
        spore_header *hd = &r.headers[i];
        REQUIRE(hd->name.len > 0 && inside(hd->name, lo, hi));
        REQUIRE(inside(hd->value, lo, hi));
        REQUIRE(!memchr(hd->value.ptr, '\r', hd->value.len) &&
                !memchr(hd->value.ptr, '\n', hd->value.len));
        spore__host_allowed(hd->value);
        spore__origin_allowed(hd->value, NULL);
        spore__has_token(hd->value, "close");
    }
    char out[512];
    long n = spore_url_decode(r.path.ptr, r.path.len, out, sizeof out);
    REQUIRE(n < (long)sizeof out && (n < 0 || out[n] == '\0'));
    n = spore_query_get(&r, "a", out, sizeof out);
    REQUIRE(n < (long)sizeof out);
    spore_header_get(&r, "Host");
    return 0;
}
