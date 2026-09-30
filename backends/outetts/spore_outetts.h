/* spore_outetts: OuteTTS 0.2/0.3 text-to-speech for spore_realtime, on
 * llama.cpp: a text-to-codes model plus the WavTokenizer vocoder.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_OUTETTS_H
#define SPORE_OUTETTS_H

#include "spore_realtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPORE_OUTETTS_RATE 24000

typedef struct spore_outetts spore_outetts;

typedef struct {
    const char *model_path;   /* OuteTTS-0.3-1B (or 0.2) GGUF */
    const char *vocoder_path; /* WavTokenizer-Large-75 GGUF */
    int n_threads;            /* 0: physical cores (hardware threads / 2), max 16 */
    int n_gpu_layers;
    int speaker_words;        /* 0: all 30. Fewer shorten every prompt: 10
                                 words cut it from ~880 to ~330 tokens */
    int chunk_codes;          /* streaming: codes per emitted chunk (75 per
                                 second); 0: 40; -1: whole sentences */
} spore_outetts_config;

/* Returns NULL on failure. Calls are serialised internally. English only:
 * text is reduced to lowercase ASCII words, numbers spelled out. */
spore_outetts *spore_outetts_new(const spore_outetts_config *cfg);
void spore_outetts_free(spore_outetts *t);

/* Matches spore_rt_tts.fn. `voice` is ignored: one built-in speaker. */
int spore_outetts_synthesize(void *self, const char *text, size_t len,
                             const char *voice, spore_rt_emit_audio emit,
                             void *ctx);

#ifdef __cplusplus
}
#endif
#endif
