/* spore_llm: OpenAI-compatible endpoints over a pluggable backend.
 * SPDX-License-Identifier: MIT
 *
 * Routes, registered only for the functions a backend provides:
 *   GET  /health
 *   GET  /v1/models
 *   POST /v1/chat/completions   (generate)
 *   POST /v1/completions        (generate)
 *   POST /v1/embeddings         (embed)
 *
 * Backend calls block and run on worker threads owned by this module, so
 * a backend is a plain loop with no knowledge of HTTP or the event loop.
 */
#ifndef SPORE_LLM_H
#define SPORE_LLM_H

#include "spore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spore_llm spore_llm;

typedef struct {
    const char *role;    /* "system", "user", "assistant", ... */
    const char *content; /* text parts joined with '\n' */
} spore_llm_msg;

/* Sampling defaults follow llama-server, not OpenAI. */
typedef struct {
    const spore_llm_msg *messages; /* chat; NULL for plain completion */
    size_t n_messages;
    const char *prompt;            /* completion; NULL for chat */
    int max_tokens;                /* -1: until end of generation */
    double temperature;            /* 0.8; <= 0 means greedy */
    double top_p;                  /* 0.95 */
    int top_k;                     /* 40 */
    double min_p;                  /* 0.05 */
    uint32_t seed;                 /* SPORE_LLM_SEED_RANDOM by default */
    int cache_prompt;              /* 1: may reuse a cached prefix; output can
                                      then differ from an uncached run */
} spore_llm_params;

#define SPORE_LLM_SEED_RANDOM 0xFFFFFFFFu

/* Receives generated text in arbitrary byte fragments. Returns 0 to
 * continue, nonzero to stop: the client left or a stop sequence matched.
 * A call with len == 0 emits nothing: backends use it to poll for a stop
 * during long work such as prompt evaluation. */
typedef int (*spore_llm_emit)(void *ctx, const char *text, size_t len);

typedef struct {
    int prompt_tokens;
    int completion_tokens;
    int cached_tokens; /* prompt tokens reused from a cache, if any */
    /* With SPORE_LLM_ERROR or SPORE_LLM_INVALID: why, for the client. */
    char error[160];
} spore_llm_usage;

typedef enum {
    SPORE_LLM_STOP,   /* end of generation, or emit returned nonzero */
    SPORE_LLM_LENGTH, /* max_tokens or context exhausted */
    SPORE_LLM_ERROR,
    SPORE_LLM_INVALID /* the request cannot be served as sent, e.g. a prompt
                         longer than the context: HTTP 400 */
} spore_llm_finish;

typedef struct {
    const char *model; /* id reported by /v1/models and in responses */
    void *self;
    /* NULL disables the completion routes. When a chat template opens a
     * <think> block in the prompt, emit "<think>" first, so the reasoning
     * can be separated as if the model had opened the block. */
    spore_llm_finish (*generate)(void *self, const spore_llm_params *p,
                                 spore_llm_emit emit, void *ctx,
                                 spore_llm_usage *usage);
    /* NULL disables /v1/embeddings. Writes embed_dim floats to `out` and
     * sets usage->prompt_tokens. Returns SPORE_LLM_STOP on success, else
     * SPORE_LLM_ERROR or SPORE_LLM_INVALID with usage->error set. */
    spore_llm_finish (*embed)(void *self, const char *text, size_t len,
                              float *out, spore_llm_usage *usage);
    size_t embed_dim;
} spore_llm_backend;

typedef struct {
    size_t workers; /* concurrent backend calls, 1. >1 needs a thread-safe backend */
    size_t queue;   /* waiting requests before 503, 16 */
} spore_llm_config;

/* Register the routes on `srv` and start the workers. The backend struct is
 * copied; `self` and `model` must outlive the returned handle. Free the
 * handle after the server's last spore_poll(): the routes stay registered
 * and point at it. In-flight requests are cancelled. */
spore_llm *spore_llm_new(spore_server *srv, const spore_llm_backend *be,
                         const spore_llm_config *cfg);
void spore_llm_free(spore_llm *llm);

#ifdef __cplusplus
}
#endif
#endif
