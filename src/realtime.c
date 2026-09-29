/* OpenAI Realtime API (GA) sessions over WebSocket.
 * SPDX-License-Identifier: MIT
 *
 * Threads: the loop thread parses client events, buffers input audio and
 * runs VAD. A worker thread per session runs the backend in order:
 * transcribe a committed turn, then generate and synthesize the response.
 * Both threads mutate session state and send events only under s->mu, so
 * a VAD barge-in (speech_started, then cancel) always precedes the
 * response.done it causes.
 */
#include "spore_realtime.h"
#include "internal.h"
#include "spore_json.h"
#include "spore_ws.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 24000                 /* wire format: 24 kHz mono s16le */
#define FRAME (RATE / 100)         /* VAD frame: 10 ms */
#define MS(samples) ((long)((samples) / (RATE / 1000)))
#define MAX_EVENT (16u << 20)      /* OpenAI caps an append at 15 MiB */

enum { ROLE_SYSTEM, ROLE_USER, ROLE_ASSISTANT };
enum { C_INPUT_TEXT, C_INPUT_AUDIO, C_OUTPUT_TEXT, C_OUTPUT_AUDIO };
enum { ST_COMPLETED, ST_INCOMPLETE, ST_IN_PROGRESS };
enum { TD_NONE, TD_SERVER, TD_SEMANTIC };
enum { J_TRANSCRIBE, J_RESPOND };

/* A conversation item: one message with one content part. Multi-part
 * input is joined into a single text. */
typedef struct {
    char *id;
    int role;
    int ctype;
    char *text;           /* text or transcript; NULL = none yet */
    size_t audio;         /* samples of audio content, at RATE */
    int status;
} item;

typedef struct {
    char id[40];
    char item_id[40];
    atomic_int cancel;
    const char *reason;   /* "turn_detected" or "client_cancelled" */
    int audio;            /* output modality */
    char *instructions;   /* override, or NULL */
    long max_tokens;      /* -1: "inf" */
} response;

typedef struct job {
    struct job *next;
    int kind;
    char *item_id;
    int16_t *pcm;         /* J_TRANSCRIBE */
    size_t n;
    response *r;          /* J_RESPOND */
} job;

typedef struct {
    char *instructions;
    int audio;            /* output_modalities ["audio"], else ["text"] */
    char *voice;
    double speed;
    int td;
    double threshold;
    int prefix_ms, silence_ms;
    int create_response, interrupt_response;
    char *eagerness;
    int transcription;    /* emit input transcription events */
    char *tr_model, *tr_language;
    char *noise;          /* noise_reduction type, echoed only */
    long max_tokens;      /* -1: "inf" */
} config;

struct spore_rt {
    spore_rt_backend be;
    char *instructions, *voice;
    size_t max_input;     /* samples */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int live;             /* sessions not yet freed */
};

typedef struct {
    spore_rt *rt;
    spore_ws *ws;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int refs;             /* loop + worker */
    atomic_int closed;    /* read by the worker without the lock */
    char id[40], conv_id[40];
    char *model;
    long expires;
    config cfg;
    /* input audio: samples [base, base + len) of everything appended */
    int16_t *in;
    size_t len, cap;
    uint64_t base, total, vad_pos;
    int speaking, onset;
    uint64_t speech_start;
    size_t silence;
    char *pending_id;     /* item id announced by speech_started */
    item **items;
    size_t n_items, cap_items;
    job *head, *tail;
    response *active;     /* queued or running response */
} session;

/* ---- ids and small helpers ---------------------------------------------- */

static atomic_ulong seq;

static char *new_id(const char *prefix) {
    char buf[48];
    unsigned long n = atomic_fetch_add(&seq, 1);
    snprintf(buf, sizeof buf, "%s_%08lx%08lx", prefix,
             (unsigned long)time(NULL) & 0xFFFFFFFFul, n & 0xFFFFFFFFul);
    char *p = malloc(strlen(buf) + 1);
    if (p) strcpy(p, buf);
    return p;
}

static void id_into(char out[40], const char *prefix) {
    char *p = new_id(prefix);
    snprintf(out, 40, "%s", p ? p : prefix);
    free(p);
}

static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *dup0(const char *s) { return s ? dupn(s, strlen(s)) : NULL; }

static void set_str(char **dst, const char *s) {
    char *p = dup0(s);
    free(*dst);
    *dst = p;
}

static void jstr(spore_buf *b, const char *s) {
    if (s) spore_json_str(b, s, strlen(s));
    else spore_buf_puts(b, "null");
}

/* ---- events --------------------------------------------------------------- */

static void ev_begin(spore_buf *b, const char *type) {
    char id[40];
    id_into(id, "event");
    spore_buf_printf(b, "{\"type\":\"%s\",\"event_id\":\"%s\"", type, id);
}

static void ev_send(session *s, spore_buf *b) {
    spore_buf_puts(b, "}");
    if (!b->err) spore_ws_send(s->ws, SPORE_WS_TEXT, b->ptr, b->len);
    spore_buf_free(b);
}

static void send_error(session *s, const char *code, const char *msg,
                       const char *param, const char *event_id) {
    spore_buf b = {0};
    ev_begin(&b, "error");
    spore_buf_puts(&b, ",\"error\":{\"type\":\"invalid_request_error\",\"code\":");
    jstr(&b, code);
    spore_buf_puts(&b, ",\"message\":");
    jstr(&b, msg);
    spore_buf_puts(&b, ",\"param\":");
    jstr(&b, param);
    spore_buf_puts(&b, ",\"event_id\":");
    jstr(&b, event_id);
    spore_buf_puts(&b, "}");
    ev_send(s, &b);
}

static const char *const role_name[] = {"system", "user", "assistant"};
static const char *const status_name[] = {"completed", "incomplete",
                                          "in_progress"};

static void item_json(spore_buf *b, const item *it) {
    spore_buf_puts(b, "{\"id\":");
    jstr(b, it->id);
    spore_buf_printf(b, ",\"object\":\"realtime.item\",\"type\":\"message\","
                        "\"status\":\"%s\",\"role\":\"%s\",\"content\":[",
                     status_name[it->status], role_name[it->role]);
    if (it->status != ST_IN_PROGRESS || it->text) {
        switch (it->ctype) {
        case C_INPUT_TEXT:
            spore_buf_puts(b, "{\"type\":\"input_text\",\"text\":");
            jstr(b, it->text ? it->text : "");
            break;
        case C_INPUT_AUDIO:
            spore_buf_puts(b, "{\"type\":\"input_audio\",\"transcript\":");
            jstr(b, it->text);
            break;
        case C_OUTPUT_TEXT:
            spore_buf_puts(b, "{\"type\":\"output_text\",\"text\":");
            jstr(b, it->text ? it->text : "");
            break;
        default:
            spore_buf_puts(b, "{\"type\":\"output_audio\",\"transcript\":");
            jstr(b, it->text ? it->text : "");
        }
        spore_buf_puts(b, "}");
    }
    spore_buf_puts(b, "]}");
}

static void format_json(spore_buf *b) {
    spore_buf_printf(b, "{\"type\":\"audio/pcm\",\"rate\":%d}", RATE);
}

static void session_json(spore_buf *b, session *s) {
    const config *c = &s->cfg;
    spore_buf_puts(b, "{\"type\":\"realtime\",\"object\":\"realtime.session\",\"id\":");
    jstr(b, s->id);
    spore_buf_puts(b, ",\"model\":");
    jstr(b, s->model);
    spore_buf_printf(b, ",\"output_modalities\":[\"%s\"],\"instructions\":",
                     c->audio ? "audio" : "text");
    jstr(b, c->instructions);
    spore_buf_puts(b, ",\"tools\":[],\"tool_choice\":\"auto\",\"max_output_tokens\":");
    if (c->max_tokens < 0) spore_buf_puts(b, "\"inf\"");
    else spore_buf_printf(b, "%ld", c->max_tokens);
    spore_buf_printf(b, ",\"tracing\":null,\"prompt\":null,\"include\":null,"
                        "\"expires_at\":%ld,\"audio\":{\"input\":{\"format\":",
                     s->expires);
    format_json(b);
    spore_buf_puts(b, ",\"transcription\":");
    if (c->transcription) {
        spore_buf_puts(b, "{\"model\":");
        jstr(b, c->tr_model);
        spore_buf_puts(b, ",\"language\":");
        jstr(b, c->tr_language);
        spore_buf_puts(b, "}");
    } else {
        spore_buf_puts(b, "null");
    }
    spore_buf_puts(b, ",\"noise_reduction\":");
    if (c->noise) {
        spore_buf_puts(b, "{\"type\":");
        jstr(b, c->noise);
        spore_buf_puts(b, "}");
    } else {
        spore_buf_puts(b, "null");
    }
    spore_buf_puts(b, ",\"turn_detection\":");
    if (c->td == TD_SERVER) {
        spore_buf_puts(b, "{\"type\":\"server_vad\",\"threshold\":");
        spore_json_num(b, c->threshold);
        spore_buf_printf(b, ",\"prefix_padding_ms\":%d,\"silence_duration_ms\":%d,"
                            "\"idle_timeout_ms\":null,\"create_response\":%s,"
                            "\"interrupt_response\":%s}",
                         c->prefix_ms, c->silence_ms,
                         c->create_response ? "true" : "false",
                         c->interrupt_response ? "true" : "false");
    } else if (c->td == TD_SEMANTIC) {
        spore_buf_puts(b, "{\"type\":\"semantic_vad\",\"eagerness\":");
        jstr(b, c->eagerness);
        spore_buf_printf(b, ",\"create_response\":%s,\"interrupt_response\":%s}",
                         c->create_response ? "true" : "false",
                         c->interrupt_response ? "true" : "false");
    } else {
        spore_buf_puts(b, "null");
    }
    spore_buf_puts(b, "},\"output\":{\"format\":");
    format_json(b);
    spore_buf_puts(b, ",\"voice\":");
    jstr(b, c->voice);
    spore_buf_puts(b, ",\"speed\":");
    spore_json_num(b, c->speed);
    spore_buf_puts(b, "}}}");
}

static void send_session(session *s, const char *type) {
    spore_buf b = {0};
    ev_begin(&b, type);
    spore_buf_puts(&b, ",\"session\":");
    session_json(&b, s);
    ev_send(s, &b);
}

/* ---- conversation --------------------------------------------------------- */

static void item_free(item *it) {
    if (!it) return;
    free(it->id);
    free(it->text);
    free(it);
}

static long find_item(session *s, const char *id) {
    for (size_t i = 0; i < s->n_items; i++)
        if (strcmp(s->items[i]->id, id) == 0) return (long)i;
    return -1;
}

/* Insert at `pos`; returns 0, or -1 on allocation failure. */
static int insert_item(session *s, size_t pos, item *it) {
    if (s->n_items == s->cap_items) {
        size_t cap = s->cap_items ? s->cap_items * 2 : 16;
        item **p = realloc(s->items, cap * sizeof *p);
        if (!p) return -1;
        s->items = p;
        s->cap_items = cap;
    }
    memmove(s->items + pos + 1, s->items + pos,
            (s->n_items - pos) * sizeof *s->items);
    s->items[pos] = it;
    s->n_items++;
    return 0;
}

static void send_item_event(session *s, const char *type, size_t pos) {
    spore_buf b = {0};
    ev_begin(&b, type);
    spore_buf_puts(&b, ",\"previous_item_id\":");
    jstr(&b, pos ? s->items[pos - 1]->id : NULL);
    spore_buf_puts(&b, ",\"item\":");
    item_json(&b, s->items[pos]);
    ev_send(s, &b);
}

/* ---- jobs ------------------------------------------------------------------- */

static void job_free(job *j) {
    if (!j) return;
    free(j->item_id);
    free(j->pcm);
    free(j);
}

static void push_job(session *s, job *j) {
    if (s->tail) s->tail->next = j;
    else s->head = j;
    s->tail = j;
    pthread_cond_signal(&s->cv);
}

static void response_free(response *r) {
    if (!r) return;
    free(r->instructions);
    free(r);
}

/* Queue a response. Returns 0, or -1 if one is already active. */
static int start_response(session *s, int audio, const char *instructions,
                          long max_tokens) {
    if (s->active) return -1;
    response *r = calloc(1, sizeof *r);
    job *j = calloc(1, sizeof *j);
    if (!r || !j || (instructions && !(r->instructions = dup0(instructions)))) {
        response_free(r);
        free(j);
        return -1;
    }
    id_into(r->id, "resp");
    id_into(r->item_id, "item");
    r->audio = audio;
    r->max_tokens = max_tokens;
    j->kind = J_RESPOND;
    j->r = r;
    s->active = r;
    push_job(s, j);
    return 0;
}

static void cancel_active(session *s, const char *reason) {
    if (!s->active) return;
    s->active->reason = reason;
    atomic_store(&s->active->cancel, 1);
}

/* Turn buffered samples [from, to) into a committed user audio item. */
static void commit(session *s, uint64_t from, uint64_t to, char *item_id) {
    size_t n = (size_t)(to - from);
    item *it = calloc(1, sizeof *it);
    job *j = calloc(1, sizeof *j);
    int16_t *pcm = malloc((n ? n : 1) * sizeof *pcm);
    char *jid = dup0(item_id);
    if (!it || !j || !pcm || !jid || insert_item(s, s->n_items, it)) {
        free(it);
        free(j);
        free(pcm);
        free(jid);
        free(item_id);
        send_error(s, "server_error", "out of memory", NULL, NULL);
        return;
    }
    memcpy(pcm, s->in + (from - s->base), n * sizeof *pcm);
    it->id = item_id;
    it->role = ROLE_USER;
    it->ctype = C_INPUT_AUDIO;
    it->audio = n;
    it->status = ST_COMPLETED;
    /* drop everything up to `to` */
    size_t drop = (size_t)(to - s->base);
    memmove(s->in, s->in + drop, (s->len - drop) * sizeof *s->in);
    s->len -= drop;
    s->base = to;

    size_t pos = s->n_items - 1;
    spore_buf b = {0};
    ev_begin(&b, "input_audio_buffer.committed");
    spore_buf_puts(&b, ",\"previous_item_id\":");
    jstr(&b, pos ? s->items[pos - 1]->id : NULL);
    spore_buf_puts(&b, ",\"item_id\":");
    jstr(&b, it->id);
    ev_send(s, &b);
    send_item_event(s, "conversation.item.added", pos);
    send_item_event(s, "conversation.item.done", pos);

    j->kind = J_TRANSCRIBE;
    j->item_id = jid;
    j->pcm = pcm;
    j->n = n;
    push_job(s, j);
}

/* ---- VAD -------------------------------------------------------------------- */

/* Energy VAD on 10 ms frames. `threshold` in [0, 1] maps linearly onto
 * -70..-20 dBFS (0.5 -> -45 dBFS); two loud frames start speech. This is a
 * heuristic stand-in for OpenAI's model-based VAD. */
static void vad(session *s) {
    const config *c = &s->cfg;
    double thr_db = -70 + 50 * c->threshold;
    size_t prefix = (size_t)c->prefix_ms * (RATE / 1000);
    int silence_ms = c->silence_ms;
    if (c->td == TD_SEMANTIC)
        silence_ms = c->eagerness && !strcmp(c->eagerness, "high") ? 300
                   : c->eagerness && !strcmp(c->eagerness, "low")  ? 1200
                                                                   : 600;
    size_t silence_max = (size_t)silence_ms * (RATE / 1000);

    while (s->vad_pos + FRAME <= s->total) {
        const int16_t *f = s->in + (s->vad_pos - s->base);
        double e = 0;
        for (int i = 0; i < FRAME; i++) e += (double)f[i] * f[i];
        double rms = sqrt(e / FRAME) / 32768.0;
        int loud = rms > 0 && 20 * log10(rms) > thr_db;
        s->vad_pos += FRAME;

        if (!s->speaking) {
            s->onset = loud ? s->onset + 1 : 0;
            if (s->onset < 2) continue;
            uint64_t start = s->vad_pos - 2 * FRAME;
            start = start > prefix ? start - prefix : 0;
            if (start < s->base) start = s->base;
            s->speaking = 1;
            s->silence = 0;
            s->speech_start = start;
            free(s->pending_id);
            s->pending_id = new_id("item");
            spore_buf b = {0};
            ev_begin(&b, "input_audio_buffer.speech_started");
            spore_buf_printf(&b, ",\"audio_start_ms\":%ld,\"item_id\":", MS(start));
            jstr(&b, s->pending_id);
            ev_send(s, &b);
            if (c->interrupt_response) cancel_active(s, "turn_detected");
        } else {
            s->silence = loud ? 0 : s->silence + FRAME;
            if (s->silence < silence_max) continue;
            uint64_t end = s->vad_pos;
            s->speaking = 0;
            s->onset = 0;
            spore_buf b = {0};
            ev_begin(&b, "input_audio_buffer.speech_stopped");
            spore_buf_printf(&b, ",\"audio_end_ms\":%ld,\"item_id\":", MS(end));
            jstr(&b, s->pending_id);
            ev_send(s, &b);
            char *id = s->pending_id;
            s->pending_id = NULL;
            /* the buffer may hold earlier audio; the turn starts at speech_start */
            if (s->speech_start > s->base) {
                size_t skip = (size_t)(s->speech_start - s->base);
                memmove(s->in, s->in + skip, (s->len - skip) * sizeof *s->in);
                s->len -= skip;
                s->base = s->speech_start;
            }
            commit(s, s->base, end, id);
            if (c->create_response) start_response(s, c->audio, NULL, c->max_tokens);
        }
    }
    /* While silent, keep only what a future prefix could need. */
    if (!s->speaking && s->len > 4 * (prefix + 2 * FRAME) + RATE) {
        uint64_t keep_from = s->vad_pos > prefix + 2 * FRAME
                                 ? s->vad_pos - prefix - 2 * FRAME : 0;
        if (keep_from > s->base) {
            size_t drop = (size_t)(keep_from - s->base);
            memmove(s->in, s->in + drop, (s->len - drop) * sizeof *s->in);
            s->len -= drop;
            s->base = keep_from;
        }
    }
}

/* ---- client events ---------------------------------------------------------- */

typedef struct {
    session *s;
    spore_json *d;
    const char *event_id;
} ctx;

#define ERR(code, msg, param)                                     \
    do {                                                          \
        send_error(c->s, code, msg, param, c->event_id);          \
        return;                                                   \
    } while (0)

static const spore_jnode *get(ctx *c, const spore_jnode *o, const char *k) {
    return spore_json_get(c->d, o, k);
}

static int is_str(const spore_jnode *n) { return n && n->type == SPORE_JSTR; }
static int is_null(const spore_jnode *n) { return n && n->type == SPORE_JNULL; }

/* output_modalities: ["audio"] or ["text"]. Returns 1 audio, 0 text, -1 bad. */
static int parse_modalities(ctx *c, const spore_jnode *m) {
    if (!m || m->type != SPORE_JARR || m->len != 1) return -1;
    const spore_jnode *v = spore_json_child(c->d, m);
    if (!is_str(v)) return -1;
    if (!strcmp(v->str, "audio")) return 1;
    if (!strcmp(v->str, "text")) return 0;
    return -1;
}

/* max_output_tokens: 1..4096 or "inf" (-1). Returns -2 if invalid. */
static long parse_max_tokens(const spore_jnode *n) {
    long v;
    if (is_str(n) && !strcmp(n->str, "inf")) return -1;
    if (spore_json_int(n, &v) || v < 1 || v > 4096) return -2;
    return v;
}

static int format_ok(ctx *c, const spore_jnode *f) {
    if (!f) return 1;
    if (f->type != SPORE_JOBJ) return 0;
    const spore_jnode *t = get(c, f, "type"), *r = get(c, f, "rate");
    long rate = RATE;
    if (t && (!is_str(t) || strcmp(t->str, "audio/pcm"))) return 0;
    if (r && (spore_json_int(r, &rate) || rate != RATE)) return 0;
    return 1;
}

static void on_session_update(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *o = get(c, ev, "session");
    if (!o || o->type != SPORE_JOBJ) ERR("invalid_value", "'session' must be an object", "session");
    const spore_jnode *t = get(c, o, "type");
    if (!is_str(t) || strcmp(t->str, "realtime"))
        ERR("unsupported", "only session.type \"realtime\" is supported", "session.type");

    /* Validate everything before changing anything. */
    const spore_jnode *mod = get(c, o, "output_modalities");
    int audio = mod ? parse_modalities(c, mod) : s->cfg.audio;
    if (audio < 0) ERR("invalid_value", "output_modalities must be [\"audio\"] or [\"text\"]", "session.output_modalities");
    if (audio && !s->rt->be.synthesize) ERR("unsupported", "this server has no speech synthesis", "session.output_modalities");
    const spore_jnode *mt = get(c, o, "max_output_tokens");
    long max_tokens = mt ? parse_max_tokens(mt) : s->cfg.max_tokens;
    if (max_tokens == -2) ERR("invalid_value", "max_output_tokens must be 1..4096 or \"inf\"", "session.max_output_tokens");
    const spore_jnode *tools = get(c, o, "tools");
    if (tools && tools->type == SPORE_JARR && tools->len)
        ERR("unsupported", "tools are not supported", "session.tools");
    const spore_jnode *instr = get(c, o, "instructions");
    if (instr && !is_str(instr)) ERR("invalid_value", "instructions must be a string", "session.instructions");

    const spore_jnode *au = get(c, o, "audio");
    const spore_jnode *in = get(c, au, "input"), *out = get(c, au, "output");
    if (!format_ok(c, get(c, in, "format")) || !format_ok(c, get(c, out, "format")))
        ERR("unsupported", "only {\"type\":\"audio/pcm\",\"rate\":24000} is supported", "session.audio");
    const spore_jnode *td = get(c, in, "turn_detection");
    int td_type = s->cfg.td;
    if (td && is_null(td)) {
        td_type = TD_NONE;
    } else if (td) {
        const spore_jnode *tt = get(c, td, "type");
        if (!is_str(tt)) ERR("invalid_value", "turn_detection.type is required", "session.audio.input.turn_detection.type");
        if (!strcmp(tt->str, "server_vad")) td_type = TD_SERVER;
        else if (!strcmp(tt->str, "semantic_vad")) td_type = TD_SEMANTIC;
        else ERR("invalid_value", "turn_detection.type must be server_vad or semantic_vad", "session.audio.input.turn_detection.type");
    }
    double threshold = s->cfg.threshold;
    long prefix = s->cfg.prefix_ms, silence = s->cfg.silence_ms;
    const spore_jnode *n;
    if (td && !is_null(td)) {
        if ((n = get(c, td, "threshold")) && (spore_json_double(n, &threshold) || threshold < 0 || threshold > 1))
            ERR("invalid_value", "threshold must be in [0, 1]", "session.audio.input.turn_detection.threshold");
        if ((n = get(c, td, "prefix_padding_ms")) && (spore_json_int(n, &prefix) || prefix < 0 || prefix > 10000))
            ERR("invalid_value", "prefix_padding_ms must be 0..10000", "session.audio.input.turn_detection.prefix_padding_ms");
        if ((n = get(c, td, "silence_duration_ms")) && (spore_json_int(n, &silence) || silence < 10 || silence > 10000))
            ERR("invalid_value", "silence_duration_ms must be 10..10000", "session.audio.input.turn_detection.silence_duration_ms");
    }

    /* Apply. */
    config *cf = &s->cfg;
    cf->audio = audio;
    cf->max_tokens = max_tokens;
    if (instr) set_str(&cf->instructions, instr->str);
    if (td_type != cf->td) {
        s->speaking = 0;
        s->onset = 0;
    }
    cf->td = td_type;
    cf->threshold = threshold;
    cf->prefix_ms = (int)prefix;
    cf->silence_ms = (int)silence;
    int flag;
    if (td && !spore_json_bool(get(c, td, "create_response"), &flag)) cf->create_response = flag;
    if (td && !spore_json_bool(get(c, td, "interrupt_response"), &flag)) cf->interrupt_response = flag;
    if (td && is_str(n = get(c, td, "eagerness"))) set_str(&cf->eagerness, n->str);
    const spore_jnode *tr = get(c, in, "transcription");
    if (tr && is_null(tr)) {
        cf->transcription = 0;
    } else if (tr && tr->type == SPORE_JOBJ) {
        cf->transcription = 1;
        if (is_str(n = get(c, tr, "model"))) set_str(&cf->tr_model, n->str);
        if (is_str(n = get(c, tr, "language"))) set_str(&cf->tr_language, n->str);
    }
    const spore_jnode *nr = get(c, in, "noise_reduction");
    if (nr && is_null(nr)) set_str(&cf->noise, NULL);
    else if (is_str(n = get(c, nr, "type"))) set_str(&cf->noise, n->str);
    if (is_str(n = get(c, out, "voice"))) set_str(&cf->voice, n->str);
    if ((n = get(c, out, "speed"))) spore_json_double(n, &cf->speed);
    send_session(s, "session.updated");
}

static void on_append(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *a = get(c, ev, "audio");
    if (!is_str(a)) ERR("invalid_value", "'audio' must be a base64 string", "audio");
    long bytes = spore__base64_decode((char *)a->str, a->len);
    if (bytes < 0 || bytes % 2) ERR("invalid_value", "'audio' is not base64 16-bit PCM", "audio");
    size_t n = (size_t)bytes / 2;
    if (s->len + n > s->rt->max_input)
        ERR("input_audio_buffer_full", "input audio buffer is full; commit or clear it", NULL);
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : RATE;
        while (cap < s->len + n) cap *= 2;
        int16_t *p = realloc(s->in, cap * sizeof *p);
        if (!p) ERR("server_error", "out of memory", NULL);
        s->in = p;
        s->cap = cap;
    }
    const unsigned char *raw = (const unsigned char *)a->str;
    for (size_t i = 0; i < n; i++)
        s->in[s->len + i] = (int16_t)(uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    s->len += n;
    s->total += n;
    if (s->cfg.td != TD_NONE) vad(s);
    else s->vad_pos = s->total;
}

static void on_commit(ctx *c) {
    session *s = c->s;
    if (!s->len) ERR("input_audio_buffer_commit_empty", "the input audio buffer is empty", NULL);
    char *id = s->pending_id ? s->pending_id : new_id("item");
    s->pending_id = NULL;
    s->speaking = 0;
    s->onset = 0;
    s->vad_pos = s->total;
    if (!id) ERR("server_error", "out of memory", NULL);
    commit(s, s->base, s->total, id);
}

static void on_clear(ctx *c) {
    session *s = c->s;
    s->base = s->total;
    s->vad_pos = s->total;
    s->len = 0;
    s->speaking = 0;
    s->onset = 0;
    spore_buf b = {0};
    ev_begin(&b, "input_audio_buffer.cleared");
    ev_send(s, &b);
}

static void on_item_create(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *o = get(c, ev, "item");
    if (!o || o->type != SPORE_JOBJ) ERR("invalid_value", "'item' must be an object", "item");
    const spore_jnode *t = get(c, o, "type");
    if (!is_str(t) || strcmp(t->str, "message"))
        ERR("unsupported", "only message items are supported", "item.type");
    const spore_jnode *r = get(c, o, "role"), *content = get(c, o, "content");
    int role = !is_str(r) ? -1 : !strcmp(r->str, "system") ? ROLE_SYSTEM
             : !strcmp(r->str, "user") ? ROLE_USER
             : !strcmp(r->str, "assistant") ? ROLE_ASSISTANT : -1;
    if (role < 0) ERR("invalid_value", "role must be system, user or assistant", "item.role");
    if (!content || content->type != SPORE_JARR)
        ERR("invalid_value", "'content' must be an array", "item.content");

    /* Position. */
    size_t pos = s->n_items;
    const spore_jnode *prev = get(c, ev, "previous_item_id");
    if (is_str(prev)) {
        if (!strcmp(prev->str, "root")) {
            pos = 0;
        } else {
            long i = find_item(s, prev->str);
            if (i < 0) ERR("item_not_found", "previous_item_id not found", "previous_item_id");
            pos = (size_t)i + 1;
        }
    }
    const spore_jnode *id = get(c, o, "id");
    if (id && (!is_str(id) || !id->len || id->len > 64))
        ERR("invalid_value", "item.id must be a string of 1..64 bytes", "item.id");
    if (is_str(id) && find_item(s, id->str) >= 0)
        ERR("invalid_value", "an item with this id exists", "item.id");

    /* Content: join text parts; at most one audio part to transcribe. */
    spore_buf text = {0};
    int ctype = role == ROLE_ASSISTANT ? C_OUTPUT_TEXT : C_INPUT_TEXT;
    int has_text = 0;
    int16_t *pcm = NULL;
    size_t npcm = 0;
    const char *bad = NULL;
    for (const spore_jnode *p = spore_json_child(c->d, content); p && !bad;
         p = spore_json_next(c->d, p)) {
        const spore_jnode *pt = get(c, p, "type");
        const char *ty = is_str(pt) ? pt->str : "";
        const spore_jnode *tx = get(c, p, "text");
        if ((!strcmp(ty, "input_text") && role != ROLE_ASSISTANT) ||
            (!strcmp(ty, "output_text") && role == ROLE_ASSISTANT) ||
            !strcmp(ty, "text")) {
            if (!is_str(tx)) {
                bad = "text parts need 'text'";
                break;
            }
            if (text.len) spore_buf_puts(&text, "\n");
            spore_buf_add(&text, tx->str, tx->len);
            has_text = 1;
        } else if (!strcmp(ty, "input_audio") && role == ROLE_USER && !pcm) {
            ctype = C_INPUT_AUDIO;
            const spore_jnode *tr = get(c, p, "transcript"), *au = get(c, p, "audio");
            if (is_str(tr)) {
                if (text.len) spore_buf_puts(&text, "\n");
                spore_buf_add(&text, tr->str, tr->len);
                has_text = 1;
            } else if (is_str(au)) {
                long bytes = spore__base64_decode((char *)au->str, au->len);
                if (bytes < 0 || bytes % 2) {
                    bad = "'audio' is not base64 16-bit PCM";
                    break;
                }
                npcm = (size_t)bytes / 2;
                pcm = malloc((npcm ? npcm : 1) * sizeof *pcm);
                if (!pcm) {
                    bad = "out of memory";
                    break;
                }
                const unsigned char *raw = (const unsigned char *)au->str;
                for (size_t i = 0; i < npcm; i++)
                    pcm[i] = (int16_t)(uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
            }
        } else {
            bad = "unsupported content part for this role";
        }
    }
    item *it = bad ? NULL : calloc(1, sizeof *it);
    char *iid = it ? (is_str(id) ? dup0(id->str) : new_id("item")) : NULL;
    if (bad || !it || !iid || text.err || insert_item(s, pos, it)) {
        free(it);
        free(iid);
        free(pcm);
        spore_buf_free(&text);
        ERR(bad ? "invalid_value" : "server_error", bad ? bad : "out of memory", "item.content");
    }
    it->id = iid;
    it->role = role;
    it->ctype = ctype;
    it->status = ST_COMPLETED;
    it->audio = npcm;
    it->text = has_text ? dupn(text.ptr ? text.ptr : "", text.len) : NULL;
    spore_buf_free(&text);
    send_item_event(s, "conversation.item.added", pos);
    send_item_event(s, "conversation.item.done", pos);
    if (pcm) {
        job *j = calloc(1, sizeof *j);
        char *jid = dup0(iid);
        if (!j || !jid) {
            free(j);
            free(jid);
            free(pcm);
            return;
        }
        j->kind = J_TRANSCRIBE;
        j->item_id = jid;
        j->pcm = pcm;
        j->n = npcm;
        push_job(s, j);
    }
}

static void on_item_delete(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *id = get(c, ev, "item_id");
    long i = is_str(id) ? find_item(s, id->str) : -1;
    if (i < 0) ERR("item_not_found", "item not found", "item_id");
    if (s->active && !strcmp(s->active->item_id, id->str))
        ERR("invalid_value", "cannot delete the item of an active response", "item_id");
    item *it = s->items[i];
    memmove(s->items + i, s->items + i + 1, (s->n_items - (size_t)i - 1) * sizeof *s->items);
    s->n_items--;
    spore_buf b = {0};
    ev_begin(&b, "conversation.item.deleted");
    spore_buf_puts(&b, ",\"item_id\":");
    jstr(&b, it->id);
    ev_send(s, &b);
    item_free(it);
}

static void on_item_retrieve(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *id = get(c, ev, "item_id");
    long i = is_str(id) ? find_item(s, id->str) : -1;
    if (i < 0) ERR("item_not_found", "item not found", "item_id");
    spore_buf b = {0};
    ev_begin(&b, "conversation.item.retrieved");
    spore_buf_puts(&b, ",\"item\":");
    item_json(&b, s->items[i]);
    ev_send(s, &b);
}

static void on_item_truncate(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *id = get(c, ev, "item_id");
    long i = is_str(id) ? find_item(s, id->str) : -1, ci, end_ms;
    if (i < 0) ERR("item_not_found", "item not found", "item_id");
    item *it = s->items[i];
    if (it->role != ROLE_ASSISTANT || it->ctype != C_OUTPUT_AUDIO)
        ERR("invalid_value", "only assistant audio items can be truncated", "item_id");
    if (spore_json_int(get(c, ev, "content_index"), &ci) || ci != 0)
        ERR("invalid_value", "content_index must be 0", "content_index");
    if (spore_json_int(get(c, ev, "audio_end_ms"), &end_ms) || end_ms < 0 ||
        end_ms > MS(it->audio))
        ERR("invalid_value", "audio_end_ms exceeds the audio duration", "audio_end_ms");
    it->audio = (size_t)end_ms * (RATE / 1000);
    set_str(&it->text, ""); /* unheard text must not stay in context */
    it->status = ST_INCOMPLETE;
    spore_buf b = {0};
    ev_begin(&b, "conversation.item.truncated");
    spore_buf_puts(&b, ",\"item_id\":");
    jstr(&b, it->id);
    spore_buf_printf(&b, ",\"content_index\":0,\"audio_end_ms\":%ld", end_ms);
    ev_send(s, &b);
}

static void on_response_create(ctx *c, const spore_jnode *ev) {
    session *s = c->s;
    const spore_jnode *o = get(c, ev, "response");
    int audio = s->cfg.audio;
    long max_tokens = s->cfg.max_tokens;
    const char *instructions = NULL;
    if (o && o->type == SPORE_JOBJ) {
        const spore_jnode *n;
        if ((n = get(c, o, "output_modalities")) && (audio = parse_modalities(c, n)) < 0)
            ERR("invalid_value", "output_modalities must be [\"audio\"] or [\"text\"]", "response.output_modalities");
        if ((n = get(c, o, "max_output_tokens")) && (max_tokens = parse_max_tokens(n)) == -2)
            ERR("invalid_value", "max_output_tokens must be 1..4096 or \"inf\"", "response.max_output_tokens");
        if ((n = get(c, o, "instructions"))) {
            if (!is_str(n)) ERR("invalid_value", "instructions must be a string", "response.instructions");
            instructions = n->str;
        }
        if (is_str(n = get(c, o, "conversation")) && strcmp(n->str, "auto"))
            ERR("unsupported", "only conversation \"auto\" is supported", "response.conversation");
        if (get(c, o, "input"))
            ERR("unsupported", "response.input is not supported", "response.input");
        if ((n = get(c, o, "tools")) && n->type == SPORE_JARR && n->len)
            ERR("unsupported", "tools are not supported", "response.tools");
    }
    if (audio && !s->rt->be.synthesize)
        ERR("unsupported", "this server has no speech synthesis", "response.output_modalities");
    if (start_response(s, audio, instructions, max_tokens))
        ERR("conversation_already_has_active_response",
            "a response is already in progress", NULL);
}

static void on_response_cancel(ctx *c) {
    session *s = c->s;
    if (!s->active) ERR("response_cancel_not_active", "no response is in progress", NULL);
    cancel_active(s, "client_cancelled");
}

static void on_message(spore_ws *ws, int type, char *data, size_t len,
                       void *ud) {
    (void)ws;
    session *s = ud;
    spore_json d = {0};
    pthread_mutex_lock(&s->mu);
    ctx cx = {s, &d, NULL}, *c = &cx;
    if (type != SPORE_WS_TEXT) {
        send_error(s, "invalid_event", "events must be JSON text frames", NULL, NULL);
    } else if (spore_json_parse(&d, data, len) || spore_json_root(&d)->type != SPORE_JOBJ) {
        send_error(s, "invalid_json", "the event is not a JSON object", NULL, NULL);
    } else {
        const spore_jnode *ev = spore_json_root(&d);
        const spore_jnode *t = get(c, ev, "type"), *eid = get(c, ev, "event_id");
        if (is_str(eid)) c->event_id = eid->str;
        const char *ty = is_str(t) ? t->str : "";
        if (!strcmp(ty, "session.update")) on_session_update(c, ev);
        else if (!strcmp(ty, "input_audio_buffer.append")) on_append(c, ev);
        else if (!strcmp(ty, "input_audio_buffer.commit")) on_commit(c);
        else if (!strcmp(ty, "input_audio_buffer.clear")) on_clear(c);
        else if (!strcmp(ty, "conversation.item.create")) on_item_create(c, ev);
        else if (!strcmp(ty, "conversation.item.delete")) on_item_delete(c, ev);
        else if (!strcmp(ty, "conversation.item.retrieve")) on_item_retrieve(c, ev);
        else if (!strcmp(ty, "conversation.item.truncate")) on_item_truncate(c, ev);
        else if (!strcmp(ty, "response.create")) on_response_create(c, ev);
        else if (!strcmp(ty, "response.cancel")) on_response_cancel(c);
        else if (!*ty) send_error(s, "invalid_event", "the 'type' field is missing", "type", c->event_id);
        else send_error(s, "invalid_event", "unsupported event type", "type", c->event_id);
    }
    spore_json_free(&d);
    pthread_mutex_unlock(&s->mu);
}

/* ---- worker: transcription -------------------------------------------------- */

static void run_transcribe(session *s, job *j) {
    const spore_rt_backend *be = &s->rt->be;
    spore_buf text = {0};
    int ok = 0;
    if (be->transcribe) {
        float *f = malloc((j->n ? j->n : 1) * sizeof *f);
        size_t m = 0;
        float *r = NULL;
        if (f) {
            spore__pcm16_to_float(j->pcm, j->n, f);
            r = spore__resample(f, j->n, RATE, be->asr_rate, &m);
        }
        ok = r && !be->transcribe(be->self, r, m, &text) && !text.err;
        free(f);
        free(r);
    }
    pthread_mutex_lock(&s->mu);
    long i = find_item(s, j->item_id);
    if (i >= 0 && ok) set_str(&s->items[i]->text, text.ptr ? text.ptr : "");
    if (i >= 0 && s->cfg.transcription) {
        spore_buf b = {0};
        if (ok) {
            ev_begin(&b, "conversation.item.input_audio_transcription.completed");
            spore_buf_puts(&b, ",\"item_id\":");
            jstr(&b, j->item_id);
            spore_buf_puts(&b, ",\"content_index\":0,\"transcript\":");
            jstr(&b, text.ptr ? text.ptr : "");
            spore_buf_puts(&b, ",\"usage\":{\"type\":\"duration\",\"seconds\":");
            spore_json_num(&b, (double)j->n / RATE);
            spore_buf_puts(&b, "}");
        } else {
            ev_begin(&b, "conversation.item.input_audio_transcription.failed");
            spore_buf_puts(&b, ",\"item_id\":");
            jstr(&b, j->item_id);
            spore_buf_puts(&b, ",\"content_index\":0,\"error\":{\"type\":\"server_error\","
                               "\"code\":\"transcription_failed\",\"message\":\"transcription failed\"}");
        }
        ev_send(s, &b);
    }
    pthread_mutex_unlock(&s->mu);
    spore_buf_free(&text);
}

/* ---- worker: responses ------------------------------------------------------ */

typedef struct {
    session *s;
    response *r;
    item *it;           /* the assistant item */
    spore_buf text;     /* all generated text */
    size_t sent;        /* text mode: bytes sent as deltas */
    size_t spoken;      /* audio mode: bytes handed to synthesis */
    spore_buf said;     /* audio mode: transcript actually sent */
    char *voice;
    int failed;
} gen;

static int stopped(gen *g) {
    return atomic_load(&g->r->cancel) || g->s->closed;
}

static void part_event(gen *g, const char *type, const char *field,
                       const char *val, size_t len) {
    spore_buf b = {0};
    ev_begin(&b, type);
    spore_buf_puts(&b, ",\"response_id\":");
    jstr(&b, g->r->id);
    spore_buf_puts(&b, ",\"item_id\":");
    jstr(&b, g->r->item_id);
    spore_buf_puts(&b, ",\"output_index\":0,\"content_index\":0");
    if (field) {
        spore_buf_printf(&b, ",\"%s\":", field);
        spore_json_str(&b, val, len);
    }
    ev_send(g->s, &b);
}

static int emit_audio(void *vg, const float *pcm, size_t n) {
    gen *g = vg;
    if (stopped(g)) return 1;
    const spore_rt_backend *be = &g->s->rt->be;
    size_t m = 0;
    float *r = spore__resample(pcm, n, be->tts_rate, RATE, &m);
    unsigned char *raw = r ? malloc(m * 2 + 1) : NULL;
    spore_buf b64 = {0};
    if (raw) {
        spore__float_to_pcm16le(r, m, raw);
        spore__base64(&b64, raw, m * 2);
    }
    free(r);
    free(raw);
    if (!raw || b64.err) {
        spore_buf_free(&b64);
        g->failed = 1;
        return 1;
    }
    pthread_mutex_lock(&g->s->mu);
    int stop = stopped(g);
    if (!stop) {
        part_event(g, "response.output_audio.delta", "delta",
                   b64.ptr ? b64.ptr : "", b64.len);
        g->it->audio += m;
    }
    pthread_mutex_unlock(&g->s->mu);
    spore_buf_free(&b64);
    return stop;
}

/* Send the transcript for text[from, to), then synthesize it. */
static void speak(gen *g, size_t from, size_t to) {
    if (to <= from || stopped(g)) return;
    const char *t = g->text.ptr + from;
    size_t n = to - from;
    pthread_mutex_lock(&g->s->mu);
    if (!stopped(g)) {
        part_event(g, "response.output_audio_transcript.delta", "delta", t, n);
        spore_buf_add(&g->said, t, n);
    }
    pthread_mutex_unlock(&g->s->mu);
    const spore_rt_backend *be = &g->s->rt->be;
    if (be->synthesize(be->self, t, n, g->voice, emit_audio, g) && !stopped(g))
        g->failed = 1;
}

/* End of the next sentence in text[from, end), or 0 if none yet. */
static size_t sentence_end(const char *s, size_t from, size_t end) {
    for (size_t i = from; i < end; i++) {
        char ch = s[i];
        if (ch == '\n') return i + 1;
        if ((ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == ':') &&
            i + 1 < end && (s[i + 1] == ' ' || s[i + 1] == '\n'))
            return i + 1;
    }
    return end - from > 400 ? spore__utf8_cut(s, from, end) : 0;
}

static int emit_text(void *vg, const char *text, size_t len) {
    gen *g = vg;
    if (stopped(g)) return 1;
    spore_buf_add(&g->text, text, len);
    if (g->text.err) return 1;
    if (!g->r->audio) {
        size_t safe = spore__utf8_cut(g->text.ptr, g->sent, g->text.len);
        if (safe > g->sent) {
            pthread_mutex_lock(&g->s->mu);
            if (!stopped(g))
                part_event(g, "response.output_text.delta", "delta",
                           g->text.ptr + g->sent, safe - g->sent);
            pthread_mutex_unlock(&g->s->mu);
            g->sent = safe;
        }
    } else {
        size_t e;
        while ((e = sentence_end(g->text.ptr, g->spoken, g->text.len)) > g->spoken) {
            size_t from = g->spoken;
            g->spoken = e;
            speak(g, from, e);
        }
    }
    return stopped(g);
}

static void usage_json(spore_buf *b, const spore_llm_usage *u) {
    spore_buf_printf(b,
                     "{\"total_tokens\":%d,\"input_tokens\":%d,\"output_tokens\":%d,"
                     "\"input_token_details\":{\"text_tokens\":%d,\"audio_tokens\":0,"
                     "\"cached_tokens\":%d},\"output_token_details\":"
                     "{\"text_tokens\":%d,\"audio_tokens\":0}}",
                     u->prompt_tokens + u->completion_tokens, u->prompt_tokens,
                     u->completion_tokens, u->prompt_tokens, u->cached_tokens,
                     u->completion_tokens);
}

static void response_json(spore_buf *b, session *s, response *r,
                          const char *status, const char *detail_type,
                          const char *reason, item *out,
                          const spore_llm_usage *u) {
    spore_buf_puts(b, "{\"id\":");
    jstr(b, r->id);
    spore_buf_printf(b, ",\"object\":\"realtime.response\",\"status\":\"%s\","
                        "\"status_details\":", status);
    if (!detail_type) {
        spore_buf_puts(b, "null");
    } else {
        spore_buf_printf(b, "{\"type\":\"%s\"", detail_type);
        if (reason) spore_buf_printf(b, ",\"reason\":\"%s\"", reason);
        if (!strcmp(detail_type, "failed"))
            spore_buf_puts(b, ",\"error\":{\"type\":\"server_error\",\"code\":\"generation_failed\"}");
        spore_buf_puts(b, "}");
    }
    spore_buf_puts(b, ",\"output\":[");
    if (out) item_json(b, out);
    spore_buf_puts(b, "],\"conversation_id\":");
    jstr(b, s->conv_id);
    spore_buf_printf(b, ",\"output_modalities\":[\"%s\"],\"max_output_tokens\":",
                     r->audio ? "audio" : "text");
    if (r->max_tokens < 0) spore_buf_puts(b, "\"inf\"");
    else spore_buf_printf(b, "%ld", r->max_tokens);
    spore_buf_puts(b, ",\"audio\":{\"output\":{\"format\":");
    format_json(b);
    spore_buf_puts(b, ",\"voice\":");
    jstr(b, s->cfg.voice);
    spore_buf_puts(b, "}},\"metadata\":null,\"usage\":");
    if (u) usage_json(b, u);
    else spore_buf_puts(b, "null");
    spore_buf_puts(b, "}");
}

static void send_response_event(session *s, const char *type, response *r,
                                const char *status, const char *detail_type,
                                const char *reason, item *out,
                                const spore_llm_usage *u) {
    spore_buf b = {0};
    ev_begin(&b, type);
    spore_buf_puts(&b, ",\"response\":");
    response_json(&b, s, r, status, detail_type, reason, out, u);
    ev_send(s, &b);
}

/* Messages for the LLM: instructions, then every item with text. */
static spore_llm_msg *build_messages(session *s, response *r, size_t *n,
                                     char **strings) {
    spore_llm_msg *m = calloc(s->n_items + 1, sizeof *m);
    if (!m) return NULL;
    size_t k = 0;
    const char *instr = r->instructions ? r->instructions : s->cfg.instructions;
    spore_buf all = {0}; /* one copy of every string, so the lock can drop */
    size_t *offs = calloc(s->n_items + 1, sizeof *offs);
    int *roles = calloc(s->n_items + 1, sizeof *roles);
    if (!offs || !roles) {
        free(m);
        free(offs);
        free(roles);
        return NULL;
    }
    if (instr && *instr) {
        offs[k] = all.len;
        roles[k++] = ROLE_SYSTEM;
        spore_buf_add(&all, instr, strlen(instr) + 1);
    }
    for (size_t i = 0; i < s->n_items; i++) {
        item *it = s->items[i];
        if (!it->text || !*it->text || !strcmp(it->id, r->item_id)) continue;
        offs[k] = all.len;
        roles[k++] = it->role;
        spore_buf_add(&all, it->text, strlen(it->text) + 1);
    }
    if (all.err) {
        spore_buf_free(&all);
        free(m);
        free(offs);
        free(roles);
        return NULL;
    }
    for (size_t i = 0; i < k; i++) {
        m[i].role = role_name[roles[i]];
        m[i].content = all.ptr + offs[i];
    }
    free(offs);
    free(roles);
    *strings = all.ptr;
    *n = k;
    return m;
}

static void run_response(session *s, response *r) {
    const spore_rt_backend *be = &s->rt->be;
    gen g = {s, r, NULL, {0}, 0, 0, {0}, NULL, 0};
    spore_llm_usage usage = {0, 0, 0};
    spore_llm_msg *msgs = NULL;
    char *strings = NULL;
    size_t n_msgs = 0;

    pthread_mutex_lock(&s->mu);
    send_response_event(s, "response.created", r, "in_progress", NULL, NULL, NULL, NULL);
    item *it = calloc(1, sizeof *it);
    if (it && (it->id = dup0(r->item_id)) && !insert_item(s, s->n_items, it)) {
        it->role = ROLE_ASSISTANT;
        it->ctype = r->audio ? C_OUTPUT_AUDIO : C_OUTPUT_TEXT;
        it->status = ST_IN_PROGRESS;
        g.it = it;
        spore_buf b = {0};
        ev_begin(&b, "response.output_item.added");
        spore_buf_puts(&b, ",\"response_id\":");
        jstr(&b, r->id);
        spore_buf_puts(&b, ",\"output_index\":0,\"item\":");
        item_json(&b, it);
        ev_send(s, &b);
        send_item_event(s, "conversation.item.added", s->n_items - 1);
        b = (spore_buf){0};
        ev_begin(&b, "response.content_part.added");
        spore_buf_puts(&b, ",\"response_id\":");
        jstr(&b, r->id);
        spore_buf_puts(&b, ",\"item_id\":");
        jstr(&b, r->item_id);
        spore_buf_puts(&b, r->audio
            ? ",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"audio\",\"transcript\":\"\"}"
            : ",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"text\",\"text\":\"\"}");
        ev_send(s, &b);
        msgs = build_messages(s, r, &n_msgs, &strings);
        g.voice = dup0(s->cfg.voice);
    } else if (it) {
        free(it->id);
        free(it);
    }
    pthread_mutex_unlock(&s->mu);

    spore_llm_finish fin = SPORE_LLM_ERROR;
    if (g.it && msgs && n_msgs && !stopped(&g)) { /* nothing to answer: failed */
        spore_llm_params p = {.messages = msgs, .n_messages = n_msgs,
                              .max_tokens = r->max_tokens < 0 ? -1 : (int)r->max_tokens,
                              .temperature = 0.8, .top_p = 0.95, .top_k = 40,
                              .min_p = 0.05, .seed = SPORE_LLM_SEED_RANDOM,
                              .cache_prompt = 1};
        fin = be->llm.generate(be->llm.self, &p, emit_text, &g, &usage);
        if (r->audio && !stopped(&g) && g.spoken < g.text.len)
            speak(&g, g.spoken, g.text.len);
        if (!r->audio && !stopped(&g) && g.sent < g.text.len) {
            pthread_mutex_lock(&s->mu);
            part_event(&g, "response.output_text.delta", "delta",
                       g.text.ptr + g.sent, g.text.len - g.sent);
            pthread_mutex_unlock(&s->mu);
            g.sent = g.text.len;
        }
    }
    free(msgs);
    free(strings);

    pthread_mutex_lock(&s->mu);
    int cancelled = atomic_load(&r->cancel);
    const char *status = "completed", *dtype = NULL, *reason = NULL;
    if (cancelled) {
        status = dtype = "cancelled";
        reason = r->reason ? r->reason : "client_cancelled";
    } else if (!g.it || fin == SPORE_LLM_ERROR || g.failed || g.text.err) {
        status = dtype = "failed";
    } else if (fin == SPORE_LLM_LENGTH) {
        status = dtype = "incomplete";
        reason = "max_output_tokens";
    }
    if (g.it) {
        /* The item keeps only what the client received. */
        const spore_buf *kept = r->audio ? &g.said : &g.text;
        size_t klen = r->audio ? g.said.len : g.sent;
        free(g.it->text);
        g.it->text = dupn(klen ? kept->ptr : "", klen);
        g.it->status = strcmp(status, "completed") ? ST_INCOMPLETE : ST_COMPLETED;
        const char *txt = g.it->text ? g.it->text : "";
        if (r->audio) {
            part_event(&g, "response.output_audio.done", NULL, NULL, 0);
            part_event(&g, "response.output_audio_transcript.done", "transcript", txt, strlen(txt));
        } else {
            part_event(&g, "response.output_text.done", "text", txt, strlen(txt));
        }
        spore_buf b = {0};
        ev_begin(&b, "response.content_part.done");
        spore_buf_puts(&b, ",\"response_id\":");
        jstr(&b, r->id);
        spore_buf_puts(&b, ",\"item_id\":");
        jstr(&b, r->item_id);
        spore_buf_puts(&b, r->audio
            ? ",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"audio\",\"transcript\":"
            : ",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"text\",\"text\":");
        jstr(&b, txt);
        spore_buf_puts(&b, "}");
        ev_send(s, &b);
        b = (spore_buf){0};
        ev_begin(&b, "response.output_item.done");
        spore_buf_puts(&b, ",\"response_id\":");
        jstr(&b, r->id);
        spore_buf_puts(&b, ",\"output_index\":0,\"item\":");
        item_json(&b, g.it);
        ev_send(s, &b);
        long pos = find_item(s, g.it->id);
        if (pos >= 0) send_item_event(s, "conversation.item.done", (size_t)pos);
    }
    send_response_event(s, "response.done", r, status, dtype, reason, g.it, &usage);
    s->active = NULL;
    pthread_mutex_unlock(&s->mu);
    spore_buf_free(&g.text);
    spore_buf_free(&g.said);
    free(g.voice);
    response_free(r);
}

/* ---- session lifecycle ------------------------------------------------------ */

static void session_unref(session *s) {
    pthread_mutex_lock(&s->mu);
    int n = --s->refs;
    pthread_mutex_unlock(&s->mu);
    if (n) return;
    for (size_t i = 0; i < s->n_items; i++) item_free(s->items[i]);
    free(s->items);
    while (s->head) {
        job *j = s->head;
        s->head = j->next;
        if (j->r) response_free(j->r);
        job_free(j);
    }
    config *c = &s->cfg;
    free(c->instructions);
    free(c->voice);
    free(c->eagerness);
    free(c->tr_model);
    free(c->tr_language);
    free(c->noise);
    free(s->in);
    free(s->model);
    free(s->pending_id);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    spore_rt *rt = s->rt;
    free(s);
    pthread_mutex_lock(&rt->mu);
    if (!--rt->live) pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
}

static void *worker(void *arg) {
    session *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->head && !s->closed) pthread_cond_wait(&s->cv, &s->mu);
        if (s->closed) {
            pthread_mutex_unlock(&s->mu);
            break;
        }
        job *j = s->head;
        s->head = j->next;
        if (!s->head) s->tail = NULL;
        pthread_mutex_unlock(&s->mu);
        if (j->kind == J_TRANSCRIBE) run_transcribe(s, j);
        else run_response(s, j->r);
        job_free(j);
    }
    spore_ws_release(s->ws);
    session_unref(s);
    return NULL;
}

static void on_close(spore_ws *ws, int code, void *ud) {
    (void)ws;
    (void)code;
    session *s = ud;
    pthread_mutex_lock(&s->mu);
    s->closed = 1;
    cancel_active(s, "client_cancelled");
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    session_unref(s);
}

static void on_realtime(spore_req *req, spore_resp *resp, void *ud) {
    spore_rt *rt = ud;
    session *s = calloc(1, sizeof *s);
    if (!s) {
        spore_reply(resp, 500, "text/plain", "oom\n", 4);
        return;
    }
    char model[128];
    s->rt = rt;
    s->refs = 2;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    id_into(s->id, "sess");
    id_into(s->conv_id, "conv");
    s->expires = (long)time(NULL) + 3600;
    s->model = dup0(spore_query_get(req, "model", model, sizeof model) > 0
                        ? model : rt->be.llm.model);
    config *c = &s->cfg;
    c->instructions = dup0(rt->instructions);
    c->voice = dup0(rt->voice);
    c->audio = rt->be.synthesize != NULL;
    c->speed = 1;
    c->td = TD_SERVER;
    c->threshold = 0.5;
    c->prefix_ms = 300;
    c->silence_ms = 500;
    c->create_response = c->interrupt_response = 1;
    c->max_tokens = -1;
    pthread_mutex_lock(&rt->mu);
    rt->live++;
    pthread_mutex_unlock(&rt->mu);
    if (!s->model || !c->instructions || !c->voice) {
        s->refs = 1;
        session_unref(s);
        spore_reply(resp, 500, "text/plain", "oom\n", 4);
        return;
    }
    spore_ws_config wc = {.on_message = on_message, .on_close = on_close,
                          .max_message = MAX_EVENT, .protocol = "realtime"};
    if (!(s->ws = spore_ws_accept(req, resp, &wc, s))) {
        s->refs = 1;
        session_unref(s);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, worker, s)) {
        s->refs = 1; /* on_close drops the last reference */
        spore_ws_release(s->ws);
        return;
    }
    pthread_detach(t);
    pthread_mutex_lock(&s->mu);
    send_session(s, "session.created");
    pthread_mutex_unlock(&s->mu);
}

spore_rt *spore_rt_new(spore_server *srv, const spore_rt_backend *be,
                       const spore_rt_config *cfg) {
    if (!be->llm.generate || (be->transcribe && be->asr_rate <= 0) ||
        (be->synthesize && be->tts_rate <= 0))
        return NULL;
    spore_rt *rt = calloc(1, sizeof *rt);
    if (!rt) return NULL;
    rt->be = *be;
    rt->instructions = dup0(cfg && cfg->instructions ? cfg->instructions : "");
    rt->voice = dup0(cfg && cfg->voice ? cfg->voice : "marin");
    rt->max_input = (size_t)(cfg && cfg->max_input_s > 0 ? cfg->max_input_s : 600) * RATE;
    pthread_mutex_init(&rt->mu, NULL);
    pthread_cond_init(&rt->cv, NULL);
    if (!rt->instructions || !rt->voice ||
        spore_route(srv, "GET", "/v1/realtime", on_realtime, rt)) {
        spore_rt_free(rt);
        return NULL;
    }
    return rt;
}

void spore_rt_free(spore_rt *rt) {
    if (!rt) return;
    pthread_mutex_lock(&rt->mu);
    while (rt->live) pthread_cond_wait(&rt->cv, &rt->mu);
    pthread_mutex_unlock(&rt->mu);
    pthread_cond_destroy(&rt->cv);
    pthread_mutex_destroy(&rt->mu);
    free(rt->instructions);
    free(rt->voice);
    free(rt);
}
