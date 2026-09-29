/* libFuzzer target: JSON reader and writers. */
#include "spore_json.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) abort(); } while (0)

/* Independent of json.c: RFC 3629 well-formedness. */
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

/* Check invariants and re-serialise; returns the node count visited. */
static size_t walk(const spore_json *d, const spore_jnode *n, const char *lo,
                   const char *hi, spore_buf *out) {
    size_t count = 1;
    double dv;
    long lv;
    int bv;
    switch (n->type) {
    case SPORE_JNULL: spore_buf_puts(out, "null"); break;
    case SPORE_JTRUE: spore_buf_puts(out, "true"); break;
    case SPORE_JFALSE: spore_buf_puts(out, "false"); break;
    case SPORE_JNUM:
        REQUIRE(n->str >= lo && n->str + n->len <= hi && n->len > 0);
        REQUIRE(spore_json_double(n, &dv) == 0 || n->len > 127);
        spore_json_int(n, &lv);
        spore_buf_add(out, n->str, n->len);
        break;
    case SPORE_JSTR:
        REQUIRE(n->str >= lo && n->str + n->len <= hi && n->str[n->len] == '\0');
        spore_json_str(out, n->str, n->len);
        break;
    case SPORE_JARR:
    case SPORE_JOBJ: {
        int obj = n->type == SPORE_JOBJ;
        size_t kids = 0;
        spore_buf_puts(out, obj ? "{" : "[");
        for (const spore_jnode *c = spore_json_child(d, n); c;
             c = spore_json_next(d, c), kids++) {
            if (kids) spore_buf_puts(out, obj && kids % 2 ? ":" : ",");
            if (obj && kids % 2 == 0) REQUIRE(c->type == SPORE_JSTR);
            count += walk(d, c, lo, hi, out);
        }
        REQUIRE(kids == (obj ? 2 * n->len : n->len));
        spore_buf_puts(out, obj ? "}" : "]");
        if (obj) spore_json_get(d, n, "model");
        break;
    }
    }
    REQUIRE(spore_json_bool(n, &bv) == ((n->type == SPORE_JTRUE ||
                                         n->type == SPORE_JFALSE) ? 0 : -1));
    return count;
}

static void check_reader(const uint8_t *data, size_t size) {
    char *text = malloc(size ? size : 1);
    if (!text) return;
    memcpy(text, data, size);
    spore_json d;
    if (spore_json_parse(&d, text, size) == 0) {
        spore_buf out = {0};
        size_t n = walk(&d, spore_json_root(&d), text, text + size, &out);
        REQUIRE(n == d.n && !out.err);
        /* Re-serialised output must parse to the same shape. */
        spore_json d2;
        REQUIRE(spore_json_parse(&d2, out.ptr, out.len) == 0 && d2.n == d.n);
        for (size_t i = 0; i < d.n; i++)
            REQUIRE(d2.nodes[i].type == d.nodes[i].type);
        spore_json_free(&d2);
        spore_buf_free(&out);
    }
    spore_json_free(&d);
    free(text);
}

static void check_writers(const uint8_t *data, size_t size) {
    spore_buf b = {0};
    spore_json_str(&b, (const char *)data, size);
    REQUIRE(!b.err && valid_utf8((const unsigned char *)b.ptr, b.len));
    spore_json d;
    REQUIRE(spore_json_parse(&d, b.ptr, b.len) == 0);
    const spore_jnode *s = spore_json_root(&d);
    REQUIRE(s->type == SPORE_JSTR);
    if (valid_utf8(data, size))
        REQUIRE(s->len == size && memcmp(s->str, data, size) == 0);
    spore_json_free(&d);
    spore_buf_free(&b);

    if (size >= sizeof(double)) {
        double v;
        memcpy(&v, data, sizeof v);
        spore_buf nb = {0};
        spore_json_num(&nb, v);
        REQUIRE(spore_json_parse(&d, nb.ptr, nb.len) == 0);
        double back;
        if (isnan(v) || isinf(v))
            REQUIRE(spore_json_root(&d)->type == SPORE_JNULL);
        else
            REQUIRE(spore_json_double(spore_json_root(&d), &back) == 0 &&
                    back == v);
        spore_json_free(&d);
        spore_buf_free(&nb);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    check_reader(data, size);
    check_writers(data, size);
    return 0;
}
