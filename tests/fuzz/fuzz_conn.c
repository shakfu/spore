/* libFuzzer target: the connection state machine. A server serves one end
 * of a socketpair; the input is written to the other end in fuzzed splits,
 * with spore_poll() between writes. Byte 0 picks the split size, byte 1
 * the ending: free with the connection open, half-close, or full close.
 * Output must start with a status line, and after a half-close the server
 * must answer what it was sent and close the connection. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) abort(); } while (0)
#define MAX_DEFER 8
#define MAX_ROUNDS 256 /* polls allowed for the close after a half-close */

static spore_resp *deferred[MAX_DEFER];
static size_t n_deferred;

static void on_echo(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    spore_reply(resp, 200, "text/plain", req->body, req->body_len);
}

static void on_stream(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    if (spore_begin(resp, 200, "text/event-stream")) {
        spore_end(resp);
        return;
    }
    size_t half = req->body_len / 2;
    spore_write(resp, req->body, half);
    spore_sse(resp, "e", req->body + half, req->body_len - half);
    spore_end(resp);
}

/* Completed after the poll returns, as a worker thread would. */
static void on_defer(spore_req *req, spore_resp *resp, void *ud) {
    (void)req;
    (void)ud;
    if (n_deferred < MAX_DEFER) deferred[n_deferred++] = resp;
    else spore_reply(resp, 503, "text/plain", "", 0);
}

static void complete_deferred(void) {
    for (size_t i = 0; i < n_deferred; i++)
        spore_reply(deferred[i], 200, "text/plain", "late", 4);
    n_deferred = 0;
}

/* Read what the server sent; returns 1 once it closed its end. */
static int drain(int fd, spore_buf *out) {
    char tmp[4096];
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            if (out->len < 65536) spore_buf_add(out, tmp, (size_t)n);
            continue;
        }
        if (n == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == ECONNRESET) return 1;
        return 0; /* EAGAIN */
    }
}

static void round_trip(spore_server *srv, int fd, spore_buf *out, int *gone) {
    spore_poll(srv, 0);
    complete_deferred();
    spore_poll(srv, 0); /* flush the deferred replies */
    if (!*gone) *gone = drain(fd, out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    size_t step = data[0] % 64 + 1;
    int ending = data[1] % 3; /* 0: free open, 1: half-close, 2: close */
    data += 2;
    size -= 2;

    spore_config cfg = {.max_conns = 2, .max_header = 1024, .max_body = 4096,
                        .max_pending = 8192};
    spore_server *srv = spore_new(&cfg);
    REQUIRE(srv);
    REQUIRE(!spore_route(srv, NULL, "/echo", on_echo, NULL) &&
            !spore_route(srv, "POST", "/stream", on_stream, NULL) &&
            !spore_route(srv, NULL, "/defer*", on_defer, NULL));
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
    REQUIRE(spore__adopt(srv, sv[0]) == 0);

    spore_buf out = {0};
    int gone = 0;
    for (size_t off = 0; off < size && !gone;) {
        size_t n = size - off < step ? size - off : step;
        ssize_t w = send(sv[1], data + off, n, MSG_NOSIGNAL);
        if (w > 0) off += (size_t)w;
        else if (w < 0 && errno != EAGAIN && errno != EINTR) break; /* closed */
        round_trip(srv, sv[1], &out, &gone);
        if (w <= 0 && !gone) round_trip(srv, sv[1], &out, &gone);
        if (w <= 0 && off < size && !gone) break; /* server not reading */
    }

    if (ending == 1) {
        shutdown(sv[1], SHUT_WR);
        for (int i = 0; i < MAX_ROUNDS && !gone; i++)
            round_trip(srv, sv[1], &out, &gone);
        REQUIRE(gone);
    } else if (ending == 2) {
        close(sv[1]);
        sv[1] = -1;
        for (int i = 0; i < 4; i++) {
            spore_poll(srv, 0);
            complete_deferred();
        }
    }
    complete_deferred();
    spore_free(srv);
    if (sv[1] >= 0) close(sv[1]);

    REQUIRE(!out.err);
    if (out.len) REQUIRE(out.len >= 9 && memcmp(out.ptr, "HTTP/1.1 ", 9) == 0);
    spore_buf_free(&out);
    return 0;
}
