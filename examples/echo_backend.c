/* Deterministic test backend: echoes the last message word by word.
 * SPDX-License-Identifier: MIT
 *
 * Prefixes in the echoed text change behaviour, for tests:
 *   "slow:"   sleep 50 ms before each word
 *   "fail:"   return SPORE_LLM_ERROR after the first word
 *   "invalid:" return SPORE_LLM_INVALID before any output
 *   "bytes:"  emit one byte at a time, splitting UTF-8 sequences
 *   "chop:"   emit the text after the prefix one byte at a time
 */
#define _POSIX_C_SOURCE 200809L
#include "echo_backend.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define DIM 8

static int count_words(const char *s) {
    int n = 0, in = 0;
    for (; *s; s++) {
        int sp = *s == ' ' || *s == '\n' || *s == '\t';
        n += !sp && !in;
        in = !sp;
    }
    return n;
}

static spore_llm_finish generate(void *self, const spore_llm_params *p,
                                 spore_llm_emit emit, void *ctx,
                                 spore_llm_usage *u) {
    (void)self;
    const char *text = p->prompt;
    if (p->messages) {
        text = p->messages[p->n_messages - 1].content;
        for (size_t i = 0; i < p->n_messages; i++)
            u->prompt_tokens += count_words(p->messages[i].content);
    } else {
        u->prompt_tokens = count_words(text);
    }
    int slow = strncmp(text, "slow:", 5) == 0;
    int fail = strncmp(text, "fail:", 5) == 0;
    int bytes = strncmp(text, "bytes:", 6) == 0;
    if (strncmp(text, "chop:", 5) == 0) {
        text += 5;
        bytes = 1;
    }
    if (strncmp(text, "invalid:", 8) == 0) {
        snprintf(u->error, sizeof u->error, "echo: rejected '%.20s'", text);
        return SPORE_LLM_INVALID;
    }
    const char *s = text;
    while (*s) {
        if (p->max_tokens >= 0 && u->completion_tokens >= p->max_tokens)
            return SPORE_LLM_LENGTH;
        /* one "token" = leading spaces plus a word */
        const char *e = s;
        while (*e == ' ') e++;
        while (*e && *e != ' ') e++;
        if (bytes) e = s + 1;
        if (slow) nanosleep(&(struct timespec){0, 50 * 1000000L}, NULL);
        u->completion_tokens++;
        if (emit(ctx, s, (size_t)(e - s))) return SPORE_LLM_STOP;
        if (fail) {
            snprintf(u->error, sizeof u->error, "echo: failure requested");
            return SPORE_LLM_ERROR;
        }
        s = e;
    }
    return SPORE_LLM_STOP;
}

/* A fixed pseudo-embedding: byte histogram folded into DIM buckets. */
static int embed(void *self, const char *text, size_t len, float *out,
                 int *n_tokens) {
    (void)self;
    memset(out, 0, DIM * sizeof *out);
    for (size_t i = 0; i < len; i++) out[(unsigned char)text[i] % DIM] += 1.0f;
    *n_tokens = (int)len;
    return 0;
}

spore_llm_backend echo_backend(void) {
    return (spore_llm_backend){.model = "echo",
                               .generate = generate,
                               .embed = embed,
                               .embed_dim = DIM};
}
