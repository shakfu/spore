/* spore serve: static files plus the OpenAI-compatible API.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "spore.h"
#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
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
#ifdef SPORE_WITH_WHISPER
#include "spore_whisper.h"
#endif
#ifdef SPORE_WITH_OUTETTS
#include "spore_outetts.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static spore_server *g_srv;

static void on_signal(int sig) {
    (void)sig;
    spore_stop(g_srv);
}

static void on_access(const spore_access *a, void *ud) {
    (void)ud;
    fprintf(stderr, "spore: %.*s %.*s %d %llu %lldms%s%s%s\n", (int)a->method.len,
            a->method.ptr ? a->method.ptr : "", (int)a->target.len,
            a->target.ptr ? a->target.ptr : "", a->status,
            (unsigned long long)a->bytes, (long long)a->ms,
            a->aborted ? " aborted" : "", a->reason ? " " : "",
            a->reason ? a->reason : "");
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
 * `max_pending` bytes are queued, frames are dropped, or with `wait` the
 * producer pauses until on_drain. */
typedef struct {
    spore_ws *ws;
    long frames, size, interval_us, max_pending;
    int wait;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int drained, closed;
    int refs; /* thread + on_close */
} stream_job;

static void stream_unref(stream_job *j) {
    pthread_mutex_lock(&j->mu);
    int n = --j->refs;
    pthread_mutex_unlock(&j->mu);
    if (n) return;
    pthread_cond_destroy(&j->cv);
    pthread_mutex_destroy(&j->mu);
    free(j);
}

static void stream_wake(stream_job *j, int closed) {
    pthread_mutex_lock(&j->mu);
    if (closed) j->closed = 1;
    else j->drained = 1;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

static void stream_drain(spore_ws *ws, void *ud) {
    (void)ws;
    stream_wake(ud, 0);
}

static void stream_close(spore_ws *ws, int code, void *ud) {
    (void)ws;
    (void)code;
    stream_wake(ud, 1);
    stream_unref(ud);
}

static void *stream_thread(void *arg) {
    stream_job *j = arg;
    unsigned char *buf = malloc((size_t)j->size + 1);
    long sent = 0, dropped = 0, waits = 0;
    int gone = 0;
    for (long i = 0; buf && !gone && i < j->frames; i++) {
        if (j->interval_us)
            nanosleep(&(struct timespec){j->interval_us / 1000000,
                                         (j->interval_us % 1000000) * 1000},
                      NULL);
        int skip = 0;
        /* Waiting at pending > max_pending = drain_at: a drain is due. */
        while (!gone && j->max_pending &&
               spore_ws_pending(j->ws) > (size_t)j->max_pending) {
            if (!j->wait) {
                skip = 1;
                break;
            }
            pthread_mutex_lock(&j->mu);
            while (!j->drained && !j->closed) pthread_cond_wait(&j->cv, &j->mu);
            j->drained = 0;
            gone = j->closed;
            pthread_mutex_unlock(&j->mu);
            waits++;
        }
        if (gone) break;
        if (skip) {
            dropped++;
            continue;
        }
        memset(buf, (int)(i & 0xFF), (size_t)j->size);
        if (spore_ws_send(j->ws, SPORE_WS_BINARY, buf, (size_t)j->size)) break;
        sent++;
    }
    char summary[96];
    int n = j->wait ? snprintf(summary, sizeof summary,
                               "{\"sent\":%ld,\"dropped\":%ld,\"waits\":%ld}",
                               sent, dropped, waits)
                    : snprintf(summary, sizeof summary,
                               "{\"sent\":%ld,\"dropped\":%ld}", sent, dropped);
    spore_ws_send(j->ws, SPORE_WS_TEXT, summary, (size_t)n);
    spore_ws_close(j->ws, 1000, "done");
    spore_ws_release(j->ws);
    free(buf);
    stream_unref(j);
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
                           .on_close = duplex_close,
                           .protocol = "duplex"};
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
    stream_job *j = calloc(1, sizeof *j);
    if (!j) {
        spore_reply(resp, 500, "text/plain", "oom\n", 4);
        return;
    }
    j->frames = query_long(req, "frames", 100);
    j->size = query_long(req, "size", 640); /* 20 ms of 16 kHz s16 mono */
    j->interval_us = query_long(req, "interval_us", 0);
    j->max_pending = query_long(req, "max_pending", 0);
    j->wait = query_long(req, "wait", 0) != 0;
    if (j->frames < 0 || j->size < 0 || j->size > (1L << 20) ||
        j->interval_us < 0 || j->max_pending < 0) {
        free(j);
        spore_reply(resp, 400, "text/plain", "bad query\n", 10);
        return;
    }
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    j->refs = 2;
    spore_ws_config cfg = {.on_close = stream_close, .on_drain = stream_drain,
                           .drain_at = (size_t)j->max_pending};
    if (!(j->ws = spore_ws_accept(req, resp, &cfg, j))) {
        pthread_cond_destroy(&j->cv);
        pthread_mutex_destroy(&j->mu);
        free(j);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, stream_thread, j)) {
        j->refs = 1; /* no thread; on_close drops the last reference */
        spore_ws_release(j->ws);
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

static void serve_usage(FILE *f) {
    fprintf(f,
            "usage: spore serve [options]    (a link named llama-server runs this)\n"
            "  --port N           loopback TCP port (default 8080, 0 = any)\n"
            "  --host H           127.0.0.1, localhost, ::1, or a Unix socket path ending\n"
            "                     in .sock; nothing else (see docs/dev/net-security.md)\n"
            "  --unix PATH        bind a Unix socket instead of TCP\n"
            "  --api-key KEY      require 'Authorization: Bearer KEY'; other users see KEY in ps\n"
            "  --api-key-file F   read the key from F (see 'spore token'); print a URL with it\n"
            "  --origin O         also allow this Origin (repeatable)\n"
            "  --origins-only     allow only --origin values, not loopback origins\n"
            "  --allow-host H     also allow this Host name (repeatable); checked on Unix sockets too\n"
            "  --path DIR         serve static files from DIR at /\n"
#ifdef SPORE_WITH_LLM
            "  --workers N        concurrent backend calls (default 1)\n"
            "  --queue N          waiting requests before 503 (default 16)\n"
#endif
            "  --idle-ms N        keep-alive idle timeout (default 30000)\n"
            "  --request-ms N     deadline to receive a request (default 30000)\n"
            "  --grace-ms N       on SIGINT/SIGTERM, wait this long for in-flight requests (default 5000)\n"
            "  --log              print an access log line per request to stderr\n"
#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
            "  -a, --alias NAME   model id reported by /v1/models\n"
#endif
#if defined(SPORE_WITH_LLAMA) || defined(SPORE_WITH_WHISPER)
            "  --gpu              offload every engine to the GPU (needs a GPU ggml build)\n"
#endif
#ifdef SPORE_WITH_LLAMA
            "  -m, --model PATH   GGUF model for the llama.cpp backend\n"
            "  -c, --ctx-size N   context size (default 4096; 0 = the model's own, the default\n"
            "                     with --embedding)\n"
            "  -ngl, --n-gpu-layers N  layers to offload, or 'all' (default 0)\n"
            "  -t, --threads N    generation threads (default: llama.cpp's)\n"
            "  --embedding        serve /v1/embeddings instead of completions\n"
            "  -np, --parallel N  prompt-cache slots for interleaved conversations (default 4)\n"
#endif
#ifdef SPORE_WITH_WHISPER
            "  --asr PATH         whisper.cpp model for /v1/realtime speech input\n"
            "  --asr-lang L       spoken language, or auto (default en)\n"
#endif
#ifdef SPORE_WITH_OUTETTS
            "  --tts PATH         OuteTTS 0.2/0.3 model for /v1/realtime speech output\n"
            "  --vocoder PATH     WavTokenizer model for --tts\n"
            "  --tts-speaker-words N  shorter speaker profile, faster (default 30)\n"
            "  --tts-chunk N      stream N codes per chunk (default 40); -1: whole sentences\n"
#endif
#ifdef SPORE_WITH_LLM
            "Without --model the echo test backend answers.\n"
#endif
            );
}

/* --host takes llama-server's meaning, a listen address, restricted to the
 * addresses spore can bind. */
static void set_host(spore_config *cfg, const char *v) {
    size_t n = strlen(v);
    cfg->unix_path = NULL;
    if (!strcmp(v, "127.0.0.1") || !strcmp(v, "localhost")) {
        cfg->ipv6 = 0;
    } else if (!strcmp(v, "::1") || !strcmp(v, "[::1]")) {
        cfg->ipv6 = 1;
    } else if (n > 5 && !strcmp(v + n - 5, ".sock")) {
        cfg->unix_path = v;
    } else {
        fprintf(stderr, "spore: --host %s: spore listens only on 127.0.0.1, ::1 or a Unix "
                        "socket; to reach it from elsewhere see docs/dev/net-security.md\n", v);
        exit(2);
    }
}

/* One key, as written by `spore token`: a single line without spaces. */
static void read_key_file(const char *path, char *key, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "spore: --api-key-file %s: %s\n", path, strerror(errno));
        exit(1);
    }
    size_t n = fread(key, 1, cap - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    while (n && isspace((unsigned char)key[n - 1])) n--;
    key[n] = '\0';
    int ok = n && !more;
    for (size_t i = 0; i < n; i++) ok &= !isspace((unsigned char)key[i]) && key[i] != ',';
    if (!ok) {
        fprintf(stderr, "spore: --api-key-file %s: expected one key on one line\n", path);
        exit(2);
    }
}

int serve_main(int argc, char **argv) {
    spore_config cfg = {.port = 8080};
#ifdef SPORE_WITH_LLM
    spore_llm_config lcfg = {0};
#endif
#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
    const char *alias = NULL;
#endif
    const char *static_dir = NULL, *origins[16] = {0}, *hosts[16] = {0};
    const char *key_file = NULL;
    int grace_ms = 5000;
    size_t n_hosts = 0;
    char key[1024];
    size_t n_origins = 0;
#ifdef SPORE_WITH_LLAMA
    spore_llama_config mcfg = {.n_ctx = -1, .n_slots = 4};
#endif
#if defined(SPORE_WITH_LLAMA) || defined(SPORE_WITH_WHISPER)
    int gpu = 0;
#endif
#ifdef SPORE_WITH_WHISPER
    spore_whisper_config wcfg = {0};
#endif
#ifdef SPORE_WITH_OUTETTS
    spore_outetts_config tcfg = {0};
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (CLI_FLAG("-h", "--help")) {
            serve_usage(stdout);
            return 0;
        }
        else if (CLI_ARG("--port")) cfg.port = (uint16_t)cli_long(a, v, 0, 65535);
        else if (CLI_ARG("--host")) set_host(&cfg, v);
        else if (CLI_ARG("--unix")) cfg.unix_path = v;
        else if (CLI_ARG("--api-key")) {
            if (strchr(v, ',')) {
                fprintf(stderr, "spore: --api-key: spore accepts one key\n");
                return 2;
            }
            cfg.token = v;
        }
        else if (CLI_ARG("--api-key-file")) key_file = v;
        else if (CLI_ARG("--origin")) {
            if (n_origins == 15) cli_too_many(a, 15);
            origins[n_origins++] = v;
        }
        else if (CLI_FLAG("--origins-only")) cfg.origins_only = 1;
        else if (CLI_ARG("--allow-host")) {
            if (n_hosts == 15) cli_too_many(a, 15);
            hosts[n_hosts++] = v;
        }
        else if (CLI_ARG("--path")) static_dir = v;
#ifdef SPORE_WITH_LLM
        else if (CLI_ARG("--workers")) lcfg.workers = (size_t)cli_long(a, v, 1, 256);
        else if (CLI_ARG("--queue")) lcfg.queue = (size_t)cli_long(a, v, 1, 65536);
#endif
        else if (CLI_ARG("--idle-ms")) cfg.idle_ms = (int)cli_long(a, v, 1, INT_MAX);
        else if (CLI_ARG("--request-ms")) cfg.request_ms = (int)cli_long(a, v, 1, INT_MAX);
        else if (CLI_ARG("--grace-ms")) grace_ms = (int)cli_long(a, v, 0, INT_MAX);
        else if (CLI_FLAG("--log")) cfg.log = on_access;
#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
        else if (CLI_ARG("-a", "--alias")) alias = v;
#endif
#if defined(SPORE_WITH_LLAMA) || defined(SPORE_WITH_WHISPER)
        else if (CLI_FLAG("--gpu")) gpu = 1;
#endif
#ifdef SPORE_WITH_LLAMA
        else if (CLI_ARG("-m", "--model")) mcfg.model_path = v;
        else if (CLI_ARG("-c", "--ctx-size")) mcfg.n_ctx = (int)cli_long(a, v, 0, 1L << 24);
        else if (CLI_ARG("-ngl", "--gpu-layers", "--n-gpu-layers"))
            mcfg.n_gpu_layers = strcmp(v, "all") ? (int)cli_long(a, v, 0, 999) : 999;
        else if (CLI_ARG("-t", "--threads")) mcfg.n_threads = (int)cli_long(a, v, 1, 1024);
        else if (CLI_FLAG("--embedding", "--embeddings")) mcfg.embedding = 1;
        else if (CLI_ARG("-np", "--parallel")) mcfg.n_slots = (int)cli_long(a, v, 1, 64);
#endif
#ifdef SPORE_WITH_WHISPER
        else if (CLI_ARG("--asr")) wcfg.model_path = v;
        else if (CLI_ARG("--asr-lang")) wcfg.language = v;
#endif
#ifdef SPORE_WITH_OUTETTS
        else if (CLI_ARG("--tts")) tcfg.model_path = v;
        else if (CLI_ARG("--vocoder")) tcfg.vocoder_path = v;
        else if (CLI_ARG("--tts-speaker-words"))
            tcfg.speaker_words = (int)cli_long(a, v, 1, 30);
        else if (CLI_ARG("--tts-chunk")) tcfg.chunk_codes = (int)cli_long(a, v, -1, 10000);
#endif
        else cli_unknown("serve", a);
    }
    cfg.origins = origins;
    if (n_hosts) cfg.hosts = hosts;
    if (key_file) {
        if (cfg.token) {
            fprintf(stderr, "spore: --api-key and --api-key-file are exclusive\n");
            return 2;
        }
        read_key_file(key_file, key, sizeof key);
        cfg.token = key;
    }
#ifdef SPORE_WITH_LLAMA
    if (mcfg.n_ctx < 0) mcfg.n_ctx = mcfg.embedding ? 0 : 4096;
    if (gpu && !mcfg.n_gpu_layers) mcfg.n_gpu_layers = 999; /* all layers */
#endif
#ifdef SPORE_WITH_WHISPER
    wcfg.use_gpu = gpu;
#endif
#ifdef SPORE_WITH_OUTETTS
    if (gpu) tcfg.n_gpu_layers = 999;
#endif

#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
    spore_llm_backend be = echo_backend();
#endif
#ifdef SPORE_WITH_LLAMA
    spore_llm_backend *lb = NULL;
    if (mcfg.model_path) {
        if (!(lb = spore_llama_new(&mcfg))) {
            fprintf(stderr, "spore: cannot load %s\n", mcfg.model_path);
            return 1;
        }
        be = *lb;
    }
#endif
#if defined(SPORE_WITH_LLM) || defined(SPORE_WITH_REALTIME)
    if (alias) be.model = alias;
#endif
#ifdef SPORE_WITH_WHISPER
    spore_whisper *asr = NULL;
    if (wcfg.model_path && !(asr = spore_whisper_new(&wcfg))) {
        fprintf(stderr, "spore: cannot load %s\n", wcfg.model_path);
        return 1;
    }
#endif
#ifdef SPORE_WITH_OUTETTS
    spore_outetts *tts = NULL;
    if (tcfg.model_path && (!tcfg.vocoder_path || !(tts = spore_outetts_new(&tcfg)))) {
        fprintf(stderr, "spore: cannot load %s with vocoder %s\n", tcfg.model_path,
                tcfg.vocoder_path ? tcfg.vocoder_path : "(none; use --vocoder)");
        return 1;
    }
#endif

    if (!(g_srv = spore_new(&cfg))) {
        fprintf(stderr, "spore: %s\n", strerror(errno));
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
    /* Mock stages, replaced by whichever engines were loaded. */
    spore_rt_backend rtb = rt_mock_backend();
    rtb.llm = be;
#ifdef SPORE_WITH_WHISPER
    if (asr)
        rtb.asr = (spore_rt_asr){spore_whisper_transcribe, asr, SPORE_WHISPER_RATE};
#endif
#ifdef SPORE_WITH_OUTETTS
    if (tts)
        rtb.tts = (spore_rt_tts){spore_outetts_synthesize, tts, SPORE_OUTETTS_RATE};
#endif
    /* An embedding-only model cannot answer; serve no realtime then. */
    spore_rt *rt = rtb.llm.generate ? spore_rt_new(g_srv, &rtb, NULL) : NULL;
    err |= rtb.llm.generate && !rt;
#endif
    err |= spore_route(g_srv, "POST", "/echo", on_echo, NULL) ||
           (static_dir &&
            spore_route(g_srv, "GET", "/*", on_static, (void *)static_dir));
    if (err) {
        fprintf(stderr, "spore: setup failed\n");
        return 1;
    }

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (cfg.unix_path)
        printf("spore listening on unix:%s\n", cfg.unix_path);
    else
        printf("spore listening on http://%s:%u\n",
               cfg.ipv6 ? "[::1]" : "127.0.0.1", spore_port(g_srv));
    /* The fragment never reaches a server, a log or a Referer. */
    if (key_file && !cfg.unix_path)
        printf("open http://%s:%u/#token=%s\n", cfg.ipv6 ? "[::1]" : "localhost",
               spore_port(g_srv), key);
    fflush(stdout);

    int rc = spore_run(g_srv);
    if (!rc) spore_shutdown(g_srv, grace_ms);
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
#ifdef SPORE_WITH_WHISPER
    spore_whisper_free(asr);
#endif
#ifdef SPORE_WITH_OUTETTS
    spore_outetts_free(tts);
#endif
    return rc ? 1 : 0;
}
