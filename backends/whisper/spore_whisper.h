/* spore_whisper: whisper.cpp speech-to-text for spore_realtime.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_WHISPER_H
#define SPORE_WHISPER_H

#include "spore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPORE_WHISPER_RATE 16000

typedef struct spore_whisper spore_whisper;

typedef struct {
    const char *model_path; /* ggml whisper model, e.g. ggml-base.en.bin */
    const char *language;   /* ISO-639-1, or "auto"; default "en" */
    int n_threads;          /* 0: 4 */
    int use_gpu;
} spore_whisper_config;

/* Returns NULL on failure. Calls are serialised internally. */
spore_whisper *spore_whisper_new(const spore_whisper_config *cfg);
void spore_whisper_free(spore_whisper *w);

/* Matches spore_rt_backend.transcribe: `self` is the spore_whisper, `pcm`
 * mono float at SPORE_WHISPER_RATE. Appends the text to `out`. */
int spore_whisper_transcribe(void *self, const float *pcm, size_t n,
                             spore_buf *out);

#ifdef __cplusplus
}
#endif
#endif
