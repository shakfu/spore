/* spore_llama: llama.cpp backend for spore_llm.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_LLAMA_H
#define SPORE_LLAMA_H

#include "spore_llm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *model_path; /* GGUF file */
    int n_ctx;              /* context tokens; 0 = the model's training size */
    int n_gpu_layers;       /* layers to offload; 0 = CPU only */
    int n_threads;          /* 0 = llama.cpp default */
    int embedding;          /* serve embeddings instead of completions */
    int n_slots;            /* prompt-cache slots, at most 64; 0 = 1 */
} spore_llama_config;

/* Load the model. Returns NULL on failure. Calls are serialised
 * internally, so spore_llm_config.workers above 1 gains nothing. */
spore_llm_backend *spore_llama_new(const spore_llama_config *cfg);
void spore_llama_free(spore_llm_backend *be);

#ifdef __cplusplus
}
#endif
#endif
