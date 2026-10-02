/* Unit tests for the parser, JSON and helpers. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "spore_json.h"

#include <locale.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

    CHECK(spore__host_allowed(S("localhost"), NULL));
    CHECK(spore__host_allowed(S("LOCALHOST:8080"), NULL));
    CHECK(spore__host_allowed(S("127.0.0.1:1"), NULL));
    CHECK(spore__host_allowed(S("[::1]:8080"), NULL));
    CHECK(spore__host_allowed(S("app.localhost"), NULL));
    CHECK(!spore__host_allowed(S(".localhost"), NULL));
    CHECK(!spore__host_allowed(S("localhost.evil.com"), NULL));
    CHECK(!spore__host_allowed(S("evil.com"), NULL));
    CHECK(!spore__host_allowed(S("127.0.0.2"), NULL));
    CHECK(!spore__host_allowed(S("localhost:80x"), NULL));
    CHECK(!spore__host_allowed(S("[::1]x"), NULL));
    CHECK(!spore__host_allowed(S(""), NULL));
    const char *hosts[] = {"", "spore.example.com", NULL};
    CHECK(spore__host_allowed(S("Spore.Example.com"), hosts));
    CHECK(spore__host_allowed(S("spore.example.com:443"), hosts));
    CHECK(!spore__host_allowed(S("x.spore.example.com"), hosts));
    CHECK(!spore__host_allowed(S(""), hosts));

    const char *extra[] = {"https://app.example", NULL};
    CHECK(spore__origin_allowed(S("http://localhost:5173"), NULL, 1));
    CHECK(spore__origin_allowed(S("https://127.0.0.1"), NULL, 1));
    CHECK(spore__origin_allowed(S("https://app.example"), extra, 1));
    CHECK(!spore__origin_allowed(S("https://app.example.org"), extra, 1));
    CHECK(!spore__origin_allowed(S("null"), NULL, 1));
    CHECK(!spore__origin_allowed(S("http://evil.com"), NULL, 1));
    CHECK(!spore__origin_allowed(S("file://localhost"), NULL, 1));
    CHECK(!spore__origin_allowed(S("http://localhost/x"), NULL, 1));
    CHECK(!spore__origin_allowed(S("http://localhost:5173"), extra, 0));
    CHECK(spore__origin_allowed(S("https://app.example"), extra, 0));
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

#ifdef SPORE_WITH_REALTIME
static double rms(const float *x, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; i++) e += (double)x[i] * x[i];
    return sqrt(e / (double)(to - from));
}

static void test_rt_audio(void) {
    /* base64: round trip through the encoder, then malformed input */
    unsigned char raw[256];
    for (int i = 0; i < 256; i++) raw[i] = (unsigned char)i;
    for (size_t n = 0; n <= 256; n += 37) {
        spore_buf b = {0};
        spore__base64(&b, raw, n);
        spore_buf_add(&b, "", 0);
        long got = spore__base64_decode(b.ptr ? b.ptr : (char *)"", b.len);
        CHECK(got == (long)n && (!n || memcmp(b.ptr, raw, n) == 0));
        spore_buf_free(&b);
    }
    char bad1[] = "abc", bad2[] = "ab=c", bad3[] = "a===", bad4[] = "ab!d";
    CHECK(spore__base64_decode(bad1, 3) == -1);
    CHECK(spore__base64_decode(bad2, 4) == -1);
    CHECK(spore__base64_decode(bad3, 4) == -1);
    CHECK(spore__base64_decode(bad4, 4) == -1);

    /* resampling: length, DC gain, pass band, anti-aliasing */
    enum { N = 24000 };
    float *x = malloc(N * sizeof *x);
    size_t m;
    for (int i = 0; i < N; i++) x[i] = 0.5f;
    float *y = spore__resample(x, N, 24000, 16000, &m);
    CHECK(y && m == 16000 && fabs(y[m / 2] - 0.5) < 1e-4);
    free(y);
    for (int i = 0; i < N; i++) x[i] = (float)sin(2 * 3.14159265358979 * 1000 * i / 24000.0);
    y = spore__resample(x, N, 24000, 16000, &m);
    CHECK(y && fabs(rms(y, 1000, m - 1000) - sqrt(0.5)) < 0.01); /* 1 kHz passes */
    free(y);
    for (int i = 0; i < N; i++) x[i] = (float)sin(2 * 3.14159265358979 * 10000 * i / 24000.0);
    y = spore__resample(x, N, 24000, 16000, &m);
    CHECK(y && rms(y, 1000, m - 1000) < 0.01 * sqrt(0.5)); /* 10 kHz: >40 dB down */
    free(y);
    y = spore__resample(x, N, 16000, 24000, &m); /* upsampling length */
    CHECK(y && m == 36000);
    free(y);
    CHECK(spore__resample(x, N, 0, 16000, &m) == NULL);
    free(x);

    float f[3] = {1.5f, -1.5f, 0.5f};
    unsigned char pcm[6];
    spore__float_to_pcm16le(f, 3, pcm);
    CHECK(pcm[0] == 0xFF && pcm[1] == 0x7F && pcm[2] == 0x00 && pcm[3] == 0x80);
}
#endif

static void reply_ud(spore_req *req, spore_resp *resp, void *ud) {
    (void)req;
    spore_reply(resp, 200, "text/plain", ud, strlen(ud));
}

/* GET `path` on a fresh connection, running the loop on this thread.
 * Returns the status, or -1. */
static int get_status(spore_server *srv, const char *path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons(spore_port(srv)),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    char req[128], resp[256];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: localhost\r\n"
                     "Connection: close\r\n\r\n", path);
    int status = -1;
    if (send(fd, req, (size_t)n, 0) == n) {
        for (int i = 0; i < 100; i++) {
            spore_poll(srv, 10);
            ssize_t k = recv(fd, resp, sizeof resp - 1, MSG_DONTWAIT);
            if (k > 0) {
                resp[k] = '\0';
                status = atoi(resp + 9); /* "HTTP/1.1 NNN" */
                break;
            }
        }
    }
    close(fd);
    return status;
}

static void test_unroute(void) {
    spore_server *srv = spore_new(&(spore_config){0});
    CHECK(srv != NULL);
    if (!srv) return;
    char a[] = "a", b[] = "b";
    CHECK(spore_route(srv, "GET", "/a", reply_ud, a) == 0);
    CHECK(spore_route(srv, "GET", "/b", reply_ud, b) == 0);
    CHECK(spore_route(srv, "GET", "/c", reply_ud, a) == 0);
    CHECK(get_status(srv, "/a") == 200);
    spore__unroute(srv, a);
    CHECK(get_status(srv, "/a") == 404);
    CHECK(get_status(srv, "/b") == 200);
    CHECK(get_status(srv, "/c") == 404);
    spore_free(srv);
}

int main(void) {
    test_parse_head();
    test_unroute();
    test_tokens_and_hosts();
    test_url();
    test_json_read();
    test_json_write();
#ifdef SPORE_WITH_REALTIME
    test_rt_audio();
#endif
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
