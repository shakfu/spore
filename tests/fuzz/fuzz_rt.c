/* libFuzzer target: realtime client events against a detached session.
 * Input lines are events; each is handled as the loop would, then the
 * jobs it queued (transcription, responses) run inline on a stub backend.
 * Every server event must be a JSON object with string "type" and
 * "event_id", in an unmasked, unfragmented text frame. */
#include "internal.h"
#include "spore_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) abort(); } while (0)

static int transcribe(void *self, const float *pcm, size_t n, spore_buf *out) {
    (void)self;
    (void)pcm;
    spore_buf_printf(out, "heard %zu", n);
    return 0;
}

/* Echo the last message, one word per call. */
static spore_llm_finish generate(void *self, const spore_llm_params *p,
                                 spore_llm_emit emit, void *ctx,
                                 spore_llm_usage *u) {
    (void)self;
    const char *s = p->messages ? p->messages[p->n_messages - 1].content
                                : p->prompt;
    while (*s) {
        if (p->max_tokens >= 0 && u->completion_tokens >= p->max_tokens)
            return SPORE_LLM_LENGTH;
        const char *e = s + 1;
        while (*e && *e != ' ') e++;
        u->completion_tokens++;
        if (emit(ctx, s, (size_t)(e - s))) break;
        s = e;
    }
    return SPORE_LLM_STOP;
}

/* 2 ms of silence per byte, capped so long texts stay cheap. */
static int synthesize(void *self, const char *text, size_t len,
                      const char *voice, spore_rt_emit_audio emit, void *ctx) {
    (void)self;
    (void)text;
    (void)voice;
    static const float pcm[32];
    for (size_t i = 0; i < len && i < 64; i++)
        if (emit(ctx, pcm, 32)) break;
    return 0;
}

/* Check and consume the server's frames. */
static void check_output(spore__rt_sess *s) {
    spore_buf out = {0};
    spore__resp_take(spore__ws_resp(spore__rt_ws(s)), &out);
    REQUIRE(!out.err);
    unsigned char *p = (unsigned char *)out.ptr;
    size_t i = 0;
    while (i < out.len) {
        REQUIRE(out.len - i >= 2);
        REQUIRE(p[i] == 0x81 && !(p[i + 1] & 0x80)); /* FIN text, unmasked */
        size_t n = p[i + 1] & 0x7F, h = 2;
        if (n == 126) {
            REQUIRE(out.len - i >= 4);
            n = (size_t)p[i + 2] << 8 | p[i + 3];
            h = 4;
        } else if (n == 127) {
            REQUIRE(out.len - i >= 10);
            n = 0;
            for (int k = 0; k < 8; k++) n = n << 8 | p[i + 2 + k];
            h = 10;
        }
        REQUIRE(out.len - i - h >= n);
        spore_json d = {0};
        REQUIRE(spore_json_parse(&d, out.ptr + i + h, n) == 0);
        const spore_jnode *root = spore_json_root(&d);
        REQUIRE(root->type == SPORE_JOBJ);
        const spore_jnode *t = spore_json_get(&d, root, "type");
        const spore_jnode *id = spore_json_get(&d, root, "event_id");
        REQUIRE(t && t->type == SPORE_JSTR && t->len);
        REQUIRE(id && id->type == SPORE_JSTR);
        spore_json_free(&d);
        i += h + n;
    }
    spore_buf_free(&out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static spore_rt *rt;
    if (!rt) {
        spore_rt_backend be = {
            .llm = {.model = "stub", .generate = generate},
            .asr = {transcribe, NULL, 16000},
            .tts = {synthesize, NULL, 16000},
        };
        rt = spore__rt_detached(&be, NULL);
        REQUIRE(rt);
    }
    spore__rt_sess *s = spore__rt_open(rt);
    if (!s) return 0;
    check_output(s);
    for (size_t off = 0; off < size;) {
        const uint8_t *nl = memchr(data + off, '\n', size - off);
        size_t len = nl ? (size_t)(nl - data) - off : size - off;
        char *ev = malloc(len ? len : 1); /* exact size: over-reads trap */
        if (!ev) break;
        memcpy(ev, data + off, len);
        spore__rt_event(s, ev, len);
        free(ev);
        check_output(s);
        off += len + 1;
    }
    spore__rt_close(s);
    return 0;
}
