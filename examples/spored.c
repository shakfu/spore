/* spored: static files plus the OpenAI-compatible API.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "spore.h"
#ifdef SPORE_WITH_LLM
#include "echo_backend.h"
#include "spore_llm.h"
#endif
#ifdef SPORE_WITH_WS
#include "spore_ws.h"
#endif
#ifdef SPORE_WITH_REALTIME
#include "rt_mock_backend.h"
#include "spore_realtime.h"
#endif
#ifdef SPORE_WITH_LLAMA
#include "spore_llama.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static spore_server *g_srv;

static void on_signal(int sig) {
    (void)sig;
    spore_stop(g_srv);
}

static void on_static(spore_req *req, spore_resp *resp, void *ud) {
    spore_serve_dir(req, resp, ud);
}

static void on_echo(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    spore_str ct = spore_header_get(req, "Content-Type");
    char type[128] = "application/octet-stream";
    if (ct.ptr && ct.len < sizeof type) {
        memcpy(type, ct.ptr, ct.len);
        type[ct.len] = '\0';
    }
    spore_reply(resp, 200, type, req->body, req->body_len);
}

#ifdef SPORE_WITH_WS
/* ---- WebSocket demos --------------------------------------------------- */

static void ws_echo_message(spore_ws *ws, int type, char *data,
                            size_t len, void *ud) {
    (void)ud;
    spore_ws_send(ws, type, data, len);
}

static void ws_echo_close(spore_ws *ws, int code, void *ud) {
    (void)code;
    (void)ud;
    spore_ws_release(ws);
}

static void on_ws_echo(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    spore_ws_config cfg = {.on_message = ws_echo_message,
                           .on_close = ws_echo_close,
                           .protocol = "echo"};
    spore_ws_accept(req, resp, &cfg, NULL);
}

/* A producer thread standing in for an audio source: `frames` binary
 * frames of `size` bytes, one every `interval_us`. While more than
 * `max_pending` bytes are queued, frames are dropped rather than queued. */
typedef struct {
    spore_ws *ws;
    long frames, size, interval_us, max_pending;
} stream_job;

static void *stream_thread(void *arg) {
    stream_job *j = arg;
    unsigned char *buf = malloc((size_t)j->size + 1);
    long sent = 0, dropped = 0;
    for (long i = 0; buf && i < j->frames; i++) {
        if (j->interval_us)
            nanosleep(&(struct timespec){j->interval_us / 1000000,
                                         (j->interval_us % 1000000) * 1000},
                      NULL);
        if (j->max_pending && spore_ws_pending(j->ws) > (size_t)j->max_pending) {
            dropped++;
            continue;
        }
        memset(buf, (int)(i & 0xFF), (size_t)j->size);
        if (spore_ws_send(j->ws, SPORE_WS_BINARY, buf, (size_t)j->size)) break;
        sent++;
    }
    char summary[96];
    int n = snprintf(summary, sizeof summary, "{\"sent\":%ld,\"dropped\":%ld}",
                     sent, dropped);
    spore_ws_send(j->ws, SPORE_WS_TEXT, summary, (size_t)n);
    spore_ws_close(j->ws, 1000, "done");
    spore_ws_release(j->ws);
    free(buf);
    free(j);
    return NULL;
}

static long query_long(spore_req *req, const char *key, long dflt) {
    char v[32];
    return spore_query_get(req, key, v, sizeof v) > 0 ? atol(v) : dflt;
}

/* Full-duplex audio pattern: speech in, processed audio out, barge-in.
 * The loop thread only copies inbound frames into a bounded queue; a
 * worker (standing in for ASR + TTS) processes them and sends results.
 * A full queue drops its oldest frame: for live audio the newest matters.
 * Text "cancel" empties the queue, and no output from before the cancel
 * is sent after its acknowledgement. */
#define DUPLEX_QCAP 64

typedef struct {
    spore_ws *ws;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct { char *data; size_t len; } q[DUPLEX_QCAP];
    size_t head, count;
    long gen;          /* bumped by each cancel */
    int inflight;      /* the worker holds a frame */
    long dropped;      /* inbound frames dropped on overflow */
    long work_us;      /* simulated processing time per frame */
    int closed;
    int refs;          /* loop callbacks + worker */
} duplex;

static void duplex_unref(duplex *d) {
    pthread_mutex_lock(&d->mu);
    int n = --d->refs;
    pthread_mutex_unlock(&d->mu);
    if (n) return;
    for (size_t i = 0; i < d->count; i++)
        free(d->q[(d->head + i) % DUPLEX_QCAP].data);
    pthread_cond_destroy(&d->cv);
    pthread_mutex_destroy(&d->mu);
    free(d);
}

static void duplex_message(spore_ws *ws, int type, char *data,
                           size_t len, void *ud) {
    duplex *d = ud;
    pthread_mutex_lock(&d->mu);
    if (type == SPORE_WS_TEXT) {
        char reply[64];
        int k = 0;
        if (len == 6 && memcmp(data, "cancel", 6) == 0) {
            k = snprintf(reply, sizeof reply, "{\"cancelled\":%zu}",
                         d->count + (size_t)d->inflight);
            for (size_t i = 0; i < d->count; i++)
                free(d->q[(d->head + i) % DUPLEX_QCAP].data);
            d->head = d->count = 0;
            d->gen++;
        } else if (len == 5 && memcmp(data, "stats", 5) == 0) {
            k = snprintf(reply, sizeof reply, "{\"dropped\":%ld}", d->dropped);
        }
        /* Sent under mu: the worker cannot slip a stale frame after it. */
        if (k > 0) spore_ws_send(ws, SPORE_WS_TEXT, reply, (size_t)k);
    } else {
        char *copy = malloc(len ? len : 1);
        if (copy) {
            memcpy(copy, data, len);
            if (d->count == DUPLEX_QCAP) { /* drop the oldest */
                free(d->q[d->head].data);
                d->head = (d->head + 1) % DUPLEX_QCAP;
                d->count--;
                d->dropped++;
            }
            size_t tail = (d->head + d->count) % DUPLEX_QCAP;
            d->q[tail].data = copy;
            d->q[tail].len = len;
            d->count++;
            pthread_cond_signal(&d->cv);
        }
    }
    pthread_mutex_unlock(&d->mu);
}

static void duplex_close(spore_ws *ws, int code, void *ud) {
    (void)ws;
    (void)code;
    duplex *d = ud;
    pthread_mutex_lock(&d->mu);
    d->closed = 1;
    pthread_cond_signal(&d->cv);
    pthread_mutex_unlock(&d->mu);
    duplex_unref(d);
}

static void *duplex_worker(void *arg) {
    duplex *d = arg;
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (!d->count && !d->closed) pthread_cond_wait(&d->cv, &d->mu);
        if (d->closed) break;
        char *frame = d->q[d->head].data;
        size_t len = d->q[d->head].len;
        d->head = (d->head + 1) % DUPLEX_QCAP;
        d->count--;
        d->inflight = 1;
        long gen = d->gen;
        pthread_mutex_unlock(&d->mu);

        /* "process": reverse the bytes, after the simulated latency */
        if (d->work_us)
            nanosleep(&(struct timespec){0, d->work_us * 1000}, NULL);
        for (size_t i = 0; i < len / 2; i++) {
            char t = frame[i];
            frame[i] = frame[len - 1 - i];
            frame[len - 1 - i] = t;
        }
        pthread_mutex_lock(&d->mu);
        if (gen == d->gen) /* not cancelled meanwhile */
            spore_ws_send(d->ws, SPORE_WS_BINARY, frame, len);
        d->inflight = 0;
        free(frame);
    }
    pthread_mutex_unlock(&d->mu);
    spore_ws_release(d->ws);
    duplex_unref(d);
    return NULL;
}

static void on_ws_duplex(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    long work_us = query_long(req, "work_us", 0);
    if (work_us < 0 || work_us >= 1000000) {
        spore_reply(resp, 400, "text/plain", "bad query\n", 10);
        return;
    }
    duplex *d = calloc(1, sizeof *d);
    if (!d) {
        spore_reply(resp, 500, "text/plain", "oom\n", 4);
        return;
    }
    d->work_us = work_us;
    d->refs = 2;
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    /* Callbacks run on this (the loop) thread, so none can fire before
     * this handler returns; d->ws is set before the worker starts. */
    spore_ws_config cfg = {.on_message = duplex_message,
                           .on_close = duplex_close};
    if (!(d->ws = spore_ws_accept(req, resp, &cfg, d))) {
        pthread_cond_destroy(&d->cv);
        pthread_mutex_destroy(&d->mu);
        free(d);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, duplex_worker, d)) {
        d->refs = 1; /* no worker; on_close drops the last reference */
        spore_ws_release(d->ws);
        return;
    }
    pthread_detach(t);
}

static void on_ws_stream(spore_req *req, spore_resp *resp, void *ud) {
    (void)ud;
    stream_job *j = malloc(sizeof *j);
    if (!j) {
        spore_reply(resp, 500, "text/plain", "oom\n", 4);
        return;
    }
    j->frames = query_long(req, "frames", 100);
    j->size = query_long(req, "size", 640); /* 20 ms of 16 kHz s16 mono */
    j->interval_us = query_long(req, "interval_us", 0);
    j->max_pending = query_long(req, "max_pending", 0);
    if (j->frames < 0 || j->size < 0 || j->size > (1L << 20) ||
        j->interval_us < 0 || j->max_pending < 0) {
        free(j);
        spore_reply(resp, 400, "text/plain", "bad query\n", 10);
        return;
    }
    /* The thread owns the only reference; on_close is not needed. */
    if (!(j->ws = spore_ws_accept(req, resp, NULL, NULL))) {
        free(j);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, stream_thread, j)) {
        spore_ws_release(j->ws);
        free(j);
        return;
    }
    pthread_detach(t);
}
#endif /* SPORE_WITH_WS */

#ifndef SPORE_WITH_LLM
static void on_health(spore_req *req, spore_resp *resp, void *ud) {
    (void)req;
    (void)ud;
    spore_reply(resp, 200, "application/json", "{\"status\":\"ok\"}", 15);
}
#endif

static const char *const modules =
#ifdef SPORE_WITH_WS
    " ws"
#endif
#ifdef SPORE_WITH_LLM
    " llm"
#endif
#ifdef SPORE_WITH_REALTIME
    " realtime"
#endif
    "";

static void usage(void) {
    fprintf(stderr,
            "usage: spored [options]\n"
            "  --port N        loopback TCP port (default 8080, 0 = any)\n"
            "  --ipv6          bind ::1 instead of 127.0.0.1\n"
            "  --unix PATH     bind a Unix socket instead of TCP\n"
            "  --token T       require 'Authorization: Bearer T'\n"
            "  --origin O      also allow this Origin (repeatable)\n"
            "  --static DIR    serve DIR at /\n"
#ifdef SPORE_WITH_LLM
            "  --workers N     concurrent backend calls (default 1)\n"
            "  --queue N       waiting requests before 503 (default 16)\n"
#endif
            "  --modules       print the compiled-in modules and exit\n"
            "  --idle-ms N     keep-alive idle timeout (default 30000)\n"
            "  --request-ms N  deadline to receive a request (default 30000)\n"
#ifdef SPORE_WITH_LLAMA
            "  --model PATH    GGUF model for the llama.cpp backend\n"
            "  --ctx N         context size (default 4096; the model's own with --embedding)\n"
            "  --gpu-layers N  layers to offload (default 0)\n"
            "  --embedding     serve /v1/embeddings instead of completions\n"
#endif
#ifdef SPORE_WITH_LLM
            "Without --model the echo test backend answers.\n"
#endif
            );
    exit(2);
}

int main(int argc, char **argv) {
    spore_config cfg = {.port = 8080};
#ifdef SPORE_WITH_LLM
    spore_llm_config lcfg = {0};
#endif
    const char *static_dir = NULL, *origins[16] = {0};
    size_t n_origins = 0;
#ifdef SPORE_WITH_LLAMA
    spore_llama_config mcfg = {.n_ctx = -1};
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && (v || (usage(), 0)) && ++i)
        if (ARG("--port")) cfg.port = (uint16_t)atoi(v);
        else if (strcmp(a, "--ipv6") == 0) cfg.ipv6 = 1;
        else if (ARG("--unix")) cfg.unix_path = v;
        else if (ARG("--token")) cfg.token = v;
        else if (ARG("--origin") && n_origins < 15) origins[n_origins++] = v;
        else if (ARG("--static")) static_dir = v;
#ifdef SPORE_WITH_LLM
        else if (ARG("--workers")) lcfg.workers = (size_t)atoi(v);
        else if (ARG("--queue")) lcfg.queue = (size_t)atoi(v);
#endif
        else if (strcmp(a, "--modules") == 0) {
            printf("core%s\n", modules);
            return 0;
        }
        else if (ARG("--idle-ms")) cfg.idle_ms = atoi(v);
        else if (ARG("--request-ms")) cfg.request_ms = atoi(v);
#ifdef SPORE_WITH_LLAMA
        else if (ARG("--model")) mcfg.model_path = v;
        else if (ARG("--ctx")) mcfg.n_ctx = atoi(v);
        else if (ARG("--gpu-layers")) mcfg.n_gpu_layers = atoi(v);
        else if (strcmp(a, "--embedding") == 0) mcfg.embedding = 1;
#endif
        else usage();
#undef ARG
    }
    cfg.origins = origins;
#ifdef SPORE_WITH_LLAMA
    if (mcfg.n_ctx < 0) mcfg.n_ctx = mcfg.embedding ? 0 : 4096;
#endif

#ifdef SPORE_WITH_LLM
    spore_llm_backend be = echo_backend();
#endif
#ifdef SPORE_WITH_LLAMA
    spore_llm_backend *lb = NULL;
    if (mcfg.model_path) {
        if (!(lb = spore_llama_new(&mcfg))) {
            fprintf(stderr, "spored: cannot load %s\n", mcfg.model_path);
            return 1;
        }
        be = *lb;
    }
#endif

    if (!(g_srv = spore_new(&cfg))) {
        fprintf(stderr, "spored: %s\n", strerror(errno));
        return 1;
    }
    int err = 0;
#ifdef SPORE_WITH_LLM
    spore_llm *llm = spore_llm_new(g_srv, &be, &lcfg);
    err |= !llm;
#else
    err |= spore_route(g_srv, "GET", "/health", on_health, NULL);
#endif
#ifdef SPORE_WITH_WS
    err |= spore_route(g_srv, "GET", "/ws/echo", on_ws_echo, NULL) ||
           spore_route(g_srv, "GET", "/ws/stream", on_ws_stream, NULL) ||
           spore_route(g_srv, "GET", "/ws/duplex", on_ws_duplex, NULL);
#endif
#ifdef SPORE_WITH_REALTIME
    spore_rt_backend rtb = rt_mock_backend();
    spore_rt *rt = spore_rt_new(g_srv, &rtb, NULL);
    err |= !rt;
#endif
    err |= spore_route(g_srv, "POST", "/echo", on_echo, NULL) ||
           (static_dir &&
            spore_route(g_srv, "GET", "/*", on_static, (void *)static_dir));
    if (err) {
        fprintf(stderr, "spored: setup failed\n");
        return 1;
    }

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (cfg.unix_path)
        printf("spored listening on unix:%s\n", cfg.unix_path);
    else
        printf("spored listening on http://%s:%u\n",
               cfg.ipv6 ? "[::1]" : "127.0.0.1", spore_port(g_srv));
    fflush(stdout);

    int rc = spore_run(g_srv);
#ifdef SPORE_WITH_LLM
    spore_llm_free(llm);
#endif
    spore_free(g_srv);
#ifdef SPORE_WITH_REALTIME
    spore_rt_free(rt); /* after spore_free: that closes the sessions */
#endif
#ifdef SPORE_WITH_LLAMA
    spore_llama_free(lb);
#endif
    return rc ? 1 : 0;
}
