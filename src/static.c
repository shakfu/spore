/* Static files from a directory.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "spore.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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

    /* Reject any ".." segment; leading and repeated '/' are harmless. */
    for (char *seg = dec; *seg;) {
        char *e = strchr(seg, '/');
        size_t len = e ? (size_t)(e - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return 400;
        seg += len + (e != NULL);
    }
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
            /* Redirect so relative links in index.html resolve. */
            spore_buf loc = {0};
            spore_buf_add(&loc, req->path.ptr, req->path.len);
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
    size_t size = (size_t)st.st_size;
    char *data = malloc(size ? size : 1);
    size_t got = 0;
    while (data && got < size) {
        ssize_t r = read(fd, data + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!data || got != size) {
        free(data);
        return 500;
    }
    spore_reply(resp, 200, spore_mime(path), data, size);
    free(data);
    return 0;
}

void spore_serve_dir(spore_req *req, spore_resp *resp, const char *root) {
    int st = serve(req, resp, root);
    if (st) fail(resp, st);
}
