/* Deterministic realtime test backend, no models.
 * SPDX-License-Identifier: MIT
 *
 *   transcribe  "heard <N> ms", N = input duration at asr_rate
 *   generate    the echo backend (repeats the last message)
 *   synthesize  a 440 Hz tone, 5 ms per byte of text, in 4-byte chunks;
 *               sleeps 30 ms per chunk when the text contains "slow"
 * The rates differ from the 24 kHz wire rate so resampling is exercised.
 */
#define _POSIX_C_SOURCE 200809L
#include "rt_mock_backend.h"
#include "echo_backend.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define ASR_RATE 16000
#define TTS_RATE 16000

static int transcribe(void *self, const float *pcm, size_t n, spore_buf *out) {
    (void)self;
    (void)pcm;
    spore_buf_printf(out, "heard %ld ms", (long)(n * 1000 / ASR_RATE));
    return 0;
}

static int contains(const char *s, size_t n, const char *word) {
    size_t w = strlen(word);
    for (size_t i = 0; i + w <= n; i++)
        if (memcmp(s + i, word, w) == 0) return 1;
    return 0;
}

static int synthesize(void *self, const char *text, size_t len,
                      const char *voice, spore_rt_emit_audio emit, void *ctx) {
    (void)self;
    (void)voice;
    int slow = contains(text, len, "slow");
    float chunk[4 * TTS_RATE / 200];
    size_t per_byte = TTS_RATE / 200, t = 0;
    for (size_t i = 0; i < len; i += 4) {
        size_t bytes = len - i < 4 ? len - i : 4, n = bytes * per_byte;
        for (size_t k = 0; k < n; k++, t++)
            chunk[k] = 0.2f * (float)sin(2 * 3.14159265358979 * 440 * t / TTS_RATE);
        if (slow) nanosleep(&(struct timespec){0, 30 * 1000000L}, NULL);
        if (emit(ctx, chunk, n)) return 0;
    }
    return 0;
}

spore_rt_backend rt_mock_backend(void) {
    return (spore_rt_backend){.llm = echo_backend(),
                              .asr = {transcribe, NULL, ASR_RATE},
                              .tts = {synthesize, NULL, TTS_RATE}};
}
