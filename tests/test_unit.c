/* Unit tests for the parser, JSON and helpers. */
#include "internal.h"
#include "spore_json.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static spore_str S(const char *s) { return (spore_str){s, strlen(s)}; }

static long parse(const char *text, spore_req *req) {
    size_t scan = 0;
    return spore__parse_head(text, strlen(text), 8192, &scan, req);
}

static void test_parse_head(void) {
    spore_req r;
    const char *ok = "GET /a/b?x=1&y=%20 HTTP/1.1\r\nHost: localhost\r\n"
                     "X-Pad:  v  \r\n\r\nBODY";
    CHECK(parse(ok, &r) == (long)(strlen(ok) - 4));
    CHECK(spore_str_eq(r.method, "GET"));
    CHECK(spore_str_eq(r.path, "/a/b"));
    CHECK(spore_str_eq(r.query, "x=1&y=%20"));
    CHECK(r.minor == 1);
    CHECK(r.n_headers == 2);
    CHECK(spore_str_eq(spore_header_get(&r, "x-pad"), "v"));
    CHECK(spore_str_eq(spore_header_get(&r, "HOST"), "localhost"));

    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n", &r) == 0); /* incomplete */
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) > 0 && r.minor == 0);
    /* An absent query is an empty span at the end of the target, not NULL. */
    CHECK(r.query.len == 0 && r.query.ptr == r.target.ptr + r.target.len);
    CHECK(parse("GET / HTTP/2.0\r\n\r\n", &r) == -505);
    CHECK(parse("GET  / HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET http://x/ HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n folded\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nA: b\nc\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nA: \x01\r\n\r\n", &r) == -400);
    CHECK(parse("G\x7f / HTTP/1.1\r\n\r\n", &r) == -400);

    /* incremental scan across split reads */
    const char *full = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    size_t scan = 0;
    for (size_t n = 1; n < strlen(full); n++)
        CHECK(spore__parse_head(full, n, 8192, &scan, &r) == 0);
    CHECK(spore__parse_head(full, strlen(full), 8192, &scan, &r) ==
          (long)strlen(full));

    /* limits */
    char big[256];
    memset(big, 'a', sizeof big);
    scan = 0;
    CHECK(spore__parse_head(big, sizeof big, 100, &scan, &r) == -431);
    char many[4096] = "GET / HTTP/1.1\r\n";
    for (int i = 0; i <= SPORE_MAX_HEADERS; i++) strcat(many, "A: b\r\n");
    strcat(many, "\r\n");
    CHECK(parse(many, &r) == -431);
}

static void test_tokens_and_hosts(void) {
    CHECK(spore__has_token(S("keep-alive, Close"), "close"));
    CHECK(!spore__has_token(S("closed"), "close"));
    CHECK(spore__has_token(S(" ,close ,"), "close"));

    CHECK(spore__host_allowed(S("localhost")));
    CHECK(spore__host_allowed(S("LOCALHOST:8080")));
    CHECK(spore__host_allowed(S("127.0.0.1:1")));
    CHECK(spore__host_allowed(S("[::1]:8080")));
    CHECK(spore__host_allowed(S("app.localhost")));
    CHECK(!spore__host_allowed(S(".localhost")));
    CHECK(!spore__host_allowed(S("localhost.evil.com")));
    CHECK(!spore__host_allowed(S("evil.com")));
    CHECK(!spore__host_allowed(S("127.0.0.2")));
    CHECK(!spore__host_allowed(S("localhost:80x")));
    CHECK(!spore__host_allowed(S("[::1]x")));
    CHECK(!spore__host_allowed(S("")));

    const char *extra[] = {"https://app.example", NULL};
    CHECK(spore__origin_allowed(S("http://localhost:5173"), NULL));
    CHECK(spore__origin_allowed(S("https://127.0.0.1"), NULL));
    CHECK(spore__origin_allowed(S("https://app.example"), extra));
    CHECK(!spore__origin_allowed(S("https://app.example.org"), extra));
    CHECK(!spore__origin_allowed(S("null"), NULL));
    CHECK(!spore__origin_allowed(S("http://evil.com"), NULL));
    CHECK(!spore__origin_allowed(S("file://localhost"), NULL));
    CHECK(!spore__origin_allowed(S("http://localhost/x"), NULL));
}

static void test_url(void) {
    char out[32];
    CHECK(spore_url_decode("a%20b+c", 7, out, sizeof out) == 5 &&
          strcmp(out, "a b+c") == 0);
    CHECK(spore_url_decode("%2", 2, out, sizeof out) == -1);
    CHECK(spore_url_decode("%zz", 3, out, sizeof out) == -1);
    CHECK(spore_url_decode("%00", 3, out, sizeof out) == -1);
    CHECK(spore_url_decode("abcd", 4, out, 4) == -1);
    CHECK(spore_url_decode("abc", 3, out, 4) == 3);

    spore_req r;
    CHECK(parse("GET /?a=1&bb=x+y%21&c HTTP/1.1\r\n\r\n", &r) > 0);
    CHECK(spore_query_get(&r, "bb", out, sizeof out) == 4 &&
          strcmp(out, "x y!") == 0);
    CHECK(spore_query_get(&r, "c", out, sizeof out) == 0);
    CHECK(spore_query_get(&r, "b", out, sizeof out) == -1);
}

static int jparse(spore_json *d, const char *text, char *buf) {
    strcpy(buf, text);
    return spore_json_parse(d, buf, strlen(buf));
}

static void test_json_read(void) {
    char buf[512];
    spore_json d;
    CHECK(jparse(&d, " {\"a\": [1, -2.5e1, true, null], \"s\": \"x\\ny\\u00e9"
                     "\\ud83d\\ude00\", \"o\": {}} ", buf) == 0);
    const spore_jnode *root = spore_json_root(&d);
    CHECK(root->type == SPORE_JOBJ && root->len == 3);
    const spore_jnode *a = spore_json_get(&d, root, "a");
    CHECK(a && a->type == SPORE_JARR && a->len == 4);
    long iv;
    double dv;
    int bv;
    const spore_jnode *e = spore_json_child(&d, a);
    CHECK(spore_json_int(e, &iv) == 0 && iv == 1);
    e = spore_json_next(&d, e);
    CHECK(spore_json_double(e, &dv) == 0 && dv == -25.0);
    CHECK(spore_json_int(e, &iv) == 0 && iv == -25);
    e = spore_json_next(&d, e);
    CHECK(spore_json_bool(e, &bv) == 0 && bv == 1);
    e = spore_json_next(&d, e);
    CHECK(e->type == SPORE_JNULL && !spore_json_next(&d, e));
    const spore_jnode *s = spore_json_get(&d, root, "s");
    CHECK(s && s->len == 9 && strcmp(s->str, "x\ny\xc3\xa9\xf0\x9f\x98\x80") == 0);
    CHECK(spore_json_get(&d, root, "o")->len == 0);
    CHECK(!spore_json_get(&d, root, "missing"));
    CHECK(!spore_json_get(&d, a, "a")); /* not an object */
    spore_json_free(&d);

    CHECK(jparse(&d, "{\"k\":\"\\ud800x\"}", buf) == 0); /* lone surrogate */
    CHECK(strcmp(spore_json_get(&d, spore_json_root(&d), "k")->str,
                 "\xef\xbf\xbdx") == 0);
    spore_json_free(&d);

    CHECK(jparse(&d, "1.5", buf) == 0 && spore_json_int(spore_json_root(&d), &iv) == -1);
    spore_json_free(&d);

    const char *bad[] = {"", "{", "[1,]", "{\"a\":1,}", "01", "1.", "-", ".5",
                         "\"a", "\"\x01\"", "\"\\x\"", "tru", "[1 2]",
                         "{\"a\" 1}", "{1:2}", "1 1", "\"\\u12\"", "nul"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = jparse(&d, bad[i], buf);
        if (rc == 0) fprintf(stderr, "accepted bad json: %s\n", bad[i]);
        CHECK(rc == -1);
        spore_json_free(&d);
    }

    char deep[200];
    memset(deep, '[', 100);
    memset(deep + 100, ']', 100);
    CHECK(spore_json_parse(&d, deep, sizeof deep) == -1); /* depth > 64 */
    spore_json_free(&d);
    memset(deep, '[', 64);
    memset(deep + 64, ']', 64);
    CHECK(spore_json_parse(&d, deep, 128) == 0);
    spore_json_free(&d);
}

static void test_json_write(void) {
    spore_buf b = {0};
    spore_json_str(&b, "a\"\\\n\x01\xc3\xa9\xff\xed\xa0\x80z", 12);
    CHECK(strcmp(b.ptr, "\"a\\\"\\\\\\n\\u0001\xc3\xa9\xef\xbf\xbd"
                        "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdz\"") == 0);
    b.len = 0;
    spore_json_num(&b, 0.7);
    spore_buf_puts(&b, ",");
    spore_json_num(&b, 1e300 * 1e300);
    spore_buf_puts(&b, ",");
    spore_json_num(&b, 42);
    CHECK(strcmp(b.ptr, "0.7,null,42") == 0);

    /* A comma-radix locale must not leak into JSON or number parsing. */
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        b.len = 0;
        spore_json_num(&b, 0.25);
        CHECK(strcmp(b.ptr, "0.25") == 0);
        char buf[16];
        spore_json d;
        double v = 0;
        CHECK(jparse(&d, "0.25", buf) == 0 &&
              spore_json_double(spore_json_root(&d), &v) == 0 && v == 0.25);
        spore_json_free(&d);
        setlocale(LC_NUMERIC, "C");
    }
    spore_buf_free(&b);
}

int main(void) {
    test_parse_head();
    test_tokens_and_hosts();
    test_url();
    test_json_read();
    test_json_write();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
