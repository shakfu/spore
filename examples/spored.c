/* spored: static files plus the OpenAI-compatible API.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "spore.h"
#include "spore_llm.h"

#include "echo_backend.h"
#ifdef SPORE_WITH_LLAMA
#include "spore_llama.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void usage(void) {
    fprintf(stderr,
            "usage: spored [options]\n"
            "  --port N        loopback TCP port (default 8080, 0 = any)\n"
            "  --ipv6          bind ::1 instead of 127.0.0.1\n"
            "  --unix PATH     bind a Unix socket instead of TCP\n"
            "  --token T       require 'Authorization: Bearer T'\n"
            "  --origin O      also allow this Origin (repeatable)\n"
            "  --static DIR    serve DIR at /\n"
            "  --workers N     concurrent backend calls (default 1)\n"
            "  --queue N       waiting requests before 503 (default 16)\n"
            "  --idle-ms N     keep-alive idle timeout (default 30000)\n"
            "  --request-ms N  deadline to receive a request (default 30000)\n"
#ifdef SPORE_WITH_LLAMA
            "  --model PATH    GGUF model for the llama.cpp backend\n"
            "  --ctx N         context size (default 4096; the model's own with --embedding)\n"
            "  --gpu-layers N  layers to offload (default 0)\n"
            "  --embedding     serve /v1/embeddings instead of completions\n"
#endif
            "Without --model the echo test backend answers.\n");
    exit(2);
}

int main(int argc, char **argv) {
    spore_config cfg = {.port = 8080};
    spore_llm_config lcfg = {0};
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
        else if (ARG("--workers")) lcfg.workers = (size_t)atoi(v);
        else if (ARG("--queue")) lcfg.queue = (size_t)atoi(v);
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

    spore_llm_backend be = echo_backend();
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
    spore_llm *llm = spore_llm_new(g_srv, &be, &lcfg);
    if (!llm || spore_route(g_srv, "POST", "/echo", on_echo, NULL) ||
        (static_dir && spore_route(g_srv, "GET", "/*", on_static,
                                   (void *)static_dir))) {
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
    spore_llm_free(llm);
    spore_free(g_srv);
#ifdef SPORE_WITH_LLAMA
    spore_llama_free(lb);
#endif
    return rc ? 1 : 0;
}
