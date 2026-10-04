/* Static files from a directory.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "spore.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const char *spore_mime(const char *path) {
    static const char *const map[][2] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"txt", "text/plain; charset=utf-8"},
        {"md", "text/markdown; charset=utf-8"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"ico", "image/x-icon"},
        {"wasm", "application/wasm"},
        {"woff2", "font/woff2"},
        {"pdf", "application/pdf"},
    };
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcmp(dot + 1, map[i][0]) == 0) return map[i][1];
    return "application/octet-stream";
}

/* Decimal digits at *p; -1 if none or past 2^62. */
static long long num(const char **p, const char *end) {
    long long v = -1;
    for (; *p < end && **p >= '0' && **p <= '9'; (*p)++) {
        v = (v < 0 ? 0 : v * 10) + (**p - '0');
        if (v > (1LL << 62)) return -1;
    }
    return v;
}

/* One "bytes=" range (RFC 9110 14.1.2) of a file of `size` bytes. Returns
 * 1 with [*first, *last], 0 to send the whole file (no range, several, or
 * malformed: all ignorable), or -1 if unsatisfiable. */
static int byte_range(spore_str h, long long size, long long *first,
                      long long *last) {
    const char *p = h.ptr, *end = h.ptr + h.len;
    if (h.len < 6 || strncasecmp(p, "bytes=", 6) != 0) return 0; /* unit: any case */
    p += 6;
    long long a = num(&p, end), b = -1;
    if (p >= end || *p != '-') return 0;
    p++;
    if (p < end) {
        b = num(&p, end);
        if (b < 0) return 0;
    }
    if (p != end) return 0; /* several ranges, or junk */
    if (a < 0) { /* suffix: the last b bytes */
        if (b < 0) return 0;
        if (b == 0 || size == 0) return -1;
        a = b > size ? 0 : size - b;
        b = size - 1;
    } else {
        if (b >= 0 && b < a) return 0;
        if (a >= size) return -1;
        if (b < 0 || b >= size) b = size - 1;
    }
    *first = a;
    *last = b;
    return 1;
}

static void fail(spore_resp *resp, int status) {
    static const char *const msg[] = {"400 Bad Request\n", "404 Not Found\n",
                                      "405 Method Not Allowed\n",
                                      "500 Internal Server Error\n"};
    const char *m = status == 400 ? msg[0] : status == 404 ? msg[1]
                  : status == 405 ? msg[2] : msg[3];
    spore_reply(resp, status, "text/plain", m, strlen(m));
}

/* Returns 0 once a reply is queued, else the error status to send. */
static int serve(spore_req *req, spore_resp *resp, const char *root) {
    if (!spore_str_eq(req->method, "GET") && !spore_str_eq(req->method, "HEAD"))
        return 405;
    spore_str rel = req->tail.ptr ? req->tail : req->path;
    char dec[PATH_MAX], path[PATH_MAX];
    long n = spore_url_decode(rel.ptr, rel.len, dec, sizeof dec);
    if (n < 0) return 400;

    /* Reject any ".." segment; leading and repeated '/' are harmless.
     * Hidden files (.env, .git/) are not served. */
    int hidden = 0;
    for (char *seg = dec; *seg;) {
        char *e = strchr(seg, '/');
        size_t len = e ? (size_t)(e - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return 400;
        hidden |= seg[0] == '.';
        seg += len + (e != NULL);
    }
    if (hidden) return 404;
    size_t rl = strlen(root);
    if (rl + 1 + (size_t)n + sizeof "/index.html" > sizeof path)
        return 400;
    memcpy(path, root, rl);
    path[rl] = '/';
    memcpy(path + rl + 1, dec, (size_t)n + 1);

    struct stat st;
    if (stat(path, &st) < 0) return 404;
    if (S_ISDIR(st.st_mode)) {
        if (!req->path.len || req->path.ptr[req->path.len - 1] != '/') {
            /* Redirect so relative links in index.html resolve. One leading
             * '/': "//x/" would be a scheme-relative URL to host x. */
            spore_str p = req->path;
            while (p.len > 1 && p.ptr[1] == '/') p.ptr++, p.len--;
            spore_buf loc = {0};
            spore_buf_add(&loc, p.ptr, p.len);
            spore_buf_puts(&loc, "/");
            int ok = !loc.err && !spore_set_header(resp, "Location", loc.ptr);
            spore_buf_free(&loc);
            if (!ok) return 500;
            spore_reply(resp, 301, NULL, NULL, 0);
            return 0;
        }
        strcat(path, "/index.html");
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 404;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return 404;
    }
    long long first = 0, last = (long long)st.st_size - 1;
    int status = 200;
    spore_str rh = spore_header_get(req, "Range");
    /* Without validators an If-Range cannot be confirmed: send it all. */
    int rk = rh.ptr && !spore_header_get(req, "If-Range").ptr
                 ? byte_range(rh, (long long)st.st_size, &first, &last)
                 : 0;
    char cr[96];
    if (rk < 0) {
        close(fd);
        snprintf(cr, sizeof cr, "bytes */%lld", (long long)st.st_size);
        spore_set_header(resp, "Content-Range", cr);
        static const char m[] = "416 Range Not Satisfiable\n";
        spore_reply(resp, 416, "text/plain", m, sizeof m - 1);
        return 0;
    }
    if (rk) {
        status = 206;
        snprintf(cr, sizeof cr, "bytes %lld-%lld/%lld", first, last,
                 (long long)st.st_size);
        spore_set_header(resp, "Content-Range", cr);
    }
    spore_set_header(resp, "Accept-Ranges", "bytes");
    /* Served files may be user content: no sniffing it into HTML. */
    spore_set_header(resp, "X-Content-Type-Options", "nosniff");
    size_t size = (size_t)(last + 1 - first);
    char *data = malloc(size ? size : 1);
    size_t got = 0;
    while (data && got < size) {
        ssize_t r = pread(fd, data + got, size - got, (off_t)first + (off_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!data || got != size) {
        free(data);
        return 500;
    }
    spore_reply(resp, status, spore_mime(path), data, size);
    free(data);
    return 0;
}

void spore_serve_dir(spore_req *req, spore_resp *resp, const char *root) {
    int st = serve(req, resp, root);
    if (st) fail(resp, st);
}
