/* whisper.cpp speech-to-text adapter.
 * SPDX-License-Identifier: MIT */
#include "spore_whisper.h"

#include "whisper.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct spore_whisper {
    struct whisper_context *ctx;
    pthread_mutex_t mu;
    char *language;
    int n_threads;
};

static void log_warnings(enum ggml_log_level level, const char *text,
                         void *ud) {
    (void)ud;
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

spore_whisper *spore_whisper_new(const spore_whisper_config *cfg) {
    spore_whisper *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    whisper_log_set(log_warnings, NULL);
    struct whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = cfg->use_gpu != 0;
    const char *lang = cfg->language ? cfg->language : "en";
    w->language = malloc(strlen(lang) + 1);
    if (w->language) strcpy(w->language, lang);
    w->n_threads = cfg->n_threads > 0 ? cfg->n_threads : 4;
    w->ctx = w->language ? whisper_init_from_file_with_params(cfg->model_path, cp)
                         : NULL;
    if (!w->ctx) {
        free(w->language);
        free(w);
        return NULL;
    }
    pthread_mutex_init(&w->mu, NULL);
    return w;
}

void spore_whisper_free(spore_whisper *w) {
    if (!w) return;
    whisper_free(w->ctx);
    pthread_mutex_destroy(&w->mu);
    free(w->language);
    free(w);
}

/* whisper emits bracketed tags such as "[BLANK_AUDIO]" or "(music)" for
 * non-speech; they are not words the user said. */
static int is_tag(const char *s) {
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    return n >= 2 && ((s[0] == '[' && s[n - 1] == ']') ||
                      (s[0] == '(' && s[n - 1] == ')'));
}

int spore_whisper_transcribe(void *self, const float *pcm, size_t n,
                             spore_buf *out) {
    spore_whisper *w = self;
    /* whisper refuses input under one second; pad with silence */
    const size_t min = SPORE_WHISPER_RATE * 11 / 10;
    float *padded = NULL;
    if (n < min) {
        if (!(padded = calloc(min, sizeof *padded))) return -1;
        memcpy(padded, pcm, n * sizeof *pcm);
        pcm = padded;
        n = min;
    }
    struct whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = w->n_threads;
    p.language = w->language;
    p.no_context = true;      /* each turn stands alone */
    p.no_timestamps = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_special = false;
    p.print_timestamps = false;
    p.suppress_blank = true;

    pthread_mutex_lock(&w->mu);
    int rc = whisper_full(w->ctx, p, pcm, (int)n);
    if (rc == 0) {
        int segs = whisper_full_n_segments(w->ctx);
        for (int i = 0; i < segs; i++) {
            const char *t = whisper_full_get_segment_text(w->ctx, i);
            if (!t || is_tag(t)) continue;
            if (!out->len) while (*t == ' ') t++; /* leading space of the first */
            spore_buf_puts(out, t);
        }
        while (out->len && out->ptr[out->len - 1] == ' ') out->ptr[--out->len] = '\0';
    }
    pthread_mutex_unlock(&w->mu);
    free(padded);
    return rc == 0 && !out->err ? 0 : -1;
}
