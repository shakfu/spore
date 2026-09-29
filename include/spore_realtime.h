/* spore_realtime: OpenAI Realtime API (GA) over WebSocket at /v1/realtime.
 * SPDX-License-Identifier: MIT
 *
 * Speech to speech as a pipeline: transcribe -> generate -> synthesize.
 * The wire format is 24 kHz mono 16-bit PCM; backends see float samples at
 * their own rates, and this module resamples. Native clients authenticate
 * with "Authorization: Bearer"; browsers with the WebSocket subprotocol
 * "openai-insecure-api-key.<token>" (see spore_config.token).
 *
 * Needs the ws module. Uses only the types of spore_llm.h, not llm.c.
 */
#ifndef SPORE_REALTIME_H
#define SPORE_REALTIME_H

#include "spore.h"
#include "spore_llm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spore_rt spore_rt;

/* Receives synthesized audio: mono float in [-1, 1] at tts_rate. Returns 0
 * to continue, nonzero to stop (response cancelled). */
typedef int (*spore_rt_emit_audio)(void *ctx, const float *pcm, size_t n);

/* All calls block and run on a per-session worker thread. With more than
 * one session they can run concurrently. */
typedef struct {
    /* Response text; the same interface spore_llm serves. */
    spore_llm_backend llm;
    /* Speech to text: append UTF-8 text to `out`. `pcm` is mono float at
     * asr_rate. Returns 0, or -1 on failure. NULL: audio input is refused. */
    int (*transcribe)(void *self, const float *pcm, size_t n, spore_buf *out);
    int asr_rate;
    /* Text to speech. Returns 0, or -1 on failure. NULL: only text output. */
    int (*synthesize)(void *self, const char *text, size_t len,
                      const char *voice, spore_rt_emit_audio emit, void *ctx);
    int tts_rate;
    void *self; /* passed to transcribe and synthesize */
} spore_rt_backend;

typedef struct {
    const char *instructions; /* default system prompt; "" */
    const char *voice;        /* default voice; "marin" */
    int max_input_s;          /* input audio buffer cap in seconds; 600 */
} spore_rt_config;

/* Register GET /v1/realtime. The backend is copied; the objects it points
 * to must outlive the handle. Returns NULL on failure. */
spore_rt *spore_rt_new(spore_server *srv, const spore_rt_backend *be,
                       const spore_rt_config *cfg);
/* Call after spore_free(): that closes every session, and this waits for
 * their workers to finish. */
void spore_rt_free(spore_rt *rt);

#ifdef __cplusplus
}
#endif
#endif
