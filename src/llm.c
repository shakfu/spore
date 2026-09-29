/* OpenAI-compatible completion, chat and embedding endpoints.
 * SPDX-License-Identifier: MIT */
#include "spore_llm.h"
#include "spore_json.h"
#include "internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STOP 16

enum { JOB_CHAT, JOB_COMPLETION, JOB_EMBED };

typedef struct job {
    struct job *next;
    int kind;
    spore_resp *resp;
    char *body; /* request copy; every string below points into it */
    spore_json doc;
    spore_llm_params p;
    spore_llm_msg *msgs;
    char **owned; /* joined content strings */
    size_t n_owned;
    const char *stop[MAX_STOP];
    size_t n_stop;
    int stream;
    int include_usage;
    const spore_jnode *input; /* embeddings */
    int base64;
    char id[48];
    long created;
    char errbuf[96];
} job;

struct spore_llm {
    spore_llm_backend be;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    job *head, *tail;
    size_t queued, max_queue;
    atomic_int shutdown;
    atomic_ulong seq;
    pthread_t *threads;
    size_t n_threads;
};

/* ---- responses -------------------------------------------------------- */

static void reply_json(spore_resp *resp, int status, spore_buf *b) {
    if (b->err) {
        static const char oom[] = "{\"error\":{\"message\":\"out of memory\","
                                  "\"type\":\"server_error\"}}";
        spore_reply(resp, 500, "application/json", oom, sizeof oom - 1);
    } else {
        spore_reply(resp, status, "application/json", b->ptr, b->len);
    }
    spore_buf_free(b);
}

static void error_body(spore_buf *b, const char *msg, int status) {
    spore_buf_puts(b, "{\"error\":{\"message\":");
    spore_json_str(b, msg, strlen(msg));
    spore_buf_printf(b, ",\"type\":\"%s\",\"code\":%d}}",
                     status < 500 ? "invalid_request_error" : "server_error",
                     status);
}

static void reply_error(spore_resp *resp, int status, const char *msg) {
    spore_buf b = {0};
    error_body(&b, msg, status);
    reply_json(resp, status, &b);
}

/* ---- request parsing -------------------------------------------------- */

static void job_free(job *j) {
    if (!j) return;
    spore_json_free(&j->doc);
    for (size_t i = 0; i < j->n_owned; i++) free(j->owned[i]);
    free(j->owned);
    free(j->msgs);
    free(j->body);
    free(j);
}

#define FAIL(m) do { *err = (m); return -1; } while (0)

static int get_num(job *j, const spore_jnode *o, const char *key,
                   double *out, double lo, double hi, const char **err) {
    const spore_jnode *n = spore_json_get(&j->doc, o, key);
    if (!n || n->type == SPORE_JNULL) return 0;
    if (spore_json_double(n, out) || *out < lo || *out > hi) {
        snprintf(j->errbuf, sizeof j->errbuf,
                 "'%s' must be a number in [%g, %g]", key, lo, hi);
        FAIL(j->errbuf);
    }
    return 0;
}

static int get_int(job *j, const spore_jnode *o, const char *key, long *out,
                   long lo, long hi, const char **err) {
    const spore_jnode *n = spore_json_get(&j->doc, o, key);
    if (!n || n->type == SPORE_JNULL) return 0;
    if (spore_json_int(n, out) || *out < lo || *out > hi) {
        snprintf(j->errbuf, sizeof j->errbuf,
                 "'%s' must be an integer in [%ld, %ld]", key, lo, hi);
        FAIL(j->errbuf);
    }
    return 0;
}

static int parse_sampling(job *j, const spore_jnode *o, const char **err) {
    const spore_json *d = &j->doc;
    spore_llm_params *p = &j->p;
    long mt = -1, tk = p->top_k, seed = -1, n = 1;
    if (get_int(j, o, "max_tokens", &mt, -1, 1L << 30, err) ||
        get_int(j, o, "max_completion_tokens", &mt, -1, 1L << 30, err) ||
        get_int(j, o, "n_predict", &mt, -1, 1L << 30, err) ||
        get_int(j, o, "top_k", &tk, 0, 1L << 20, err) ||
        get_int(j, o, "seed", &seed, -1, 0xFFFFFFFFL, err) ||
        get_int(j, o, "n", &n, 1, 1, err) ||
        get_num(j, o, "temperature", &p->temperature, 0, 100, err) ||
        get_num(j, o, "top_p", &p->top_p, 0, 1, err) ||
        get_num(j, o, "min_p", &p->min_p, 0, 1, err))
        return -1;
    p->max_tokens = (int)mt;
    p->top_k = (int)tk;
    p->seed = seed < 0 ? SPORE_LLM_SEED_RANDOM : (uint32_t)seed;

    const spore_jnode *st = spore_json_get(d, o, "stop");
    if (st && st->type == SPORE_JSTR) {
        if (st->len) j->stop[j->n_stop++] = st->str;
    } else if (st && st->type == SPORE_JARR) {
        for (const spore_jnode *e = spore_json_child(d, st); e;
             e = spore_json_next(d, e)) {
            if (e->type != SPORE_JSTR) FAIL("'stop' must hold strings");
            if (j->n_stop == MAX_STOP) FAIL("too many stop sequences");
            if (e->len) j->stop[j->n_stop++] = e->str;
        }
    } else if (st && st->type != SPORE_JNULL) {
        FAIL("'stop' must be a string or an array of strings");
    }

    const spore_jnode *cp = spore_json_get(d, o, "cache_prompt");
    if (cp && cp->type != SPORE_JNULL && spore_json_bool(cp, &p->cache_prompt))
        FAIL("'cache_prompt' must be a boolean");

    const spore_jnode *s = spore_json_get(d, o, "stream");
    if (s && s->type != SPORE_JNULL && spore_json_bool(s, &j->stream))
        FAIL("'stream' must be a boolean");
    const spore_jnode *so = spore_json_get(d, o, "stream_options");
    spore_json_bool(spore_json_get(d, so, "include_usage"), &j->include_usage);

    const spore_jnode *tools = spore_json_get(d, o, "tools");
    if (tools && tools->type == SPORE_JARR && tools->len)
        FAIL("tools are not supported");
    return 0;
}

static int own(job *j, char *s) {
    char **o = realloc(j->owned, (j->n_owned + 1) * sizeof *o);
    if (!o) return -1;
    j->owned = o;
    j->owned[j->n_owned++] = s;
    return 0;
}

/* content: string | null | [{"type":"text","text":...}, ...] */
static int content_text(job *j, const spore_jnode *c, const char **out,
                        const char **err) {
    const spore_json *d = &j->doc;
    if (!c || c->type == SPORE_JNULL) {
        *out = "";
        return 0;
    }
    if (c->type == SPORE_JSTR) {
        *out = c->str;
        return 0;
    }
    if (c->type != SPORE_JARR) FAIL("'content' must be a string or an array");
    spore_buf b = {0};
    for (const spore_jnode *part = spore_json_child(d, c); part;
         part = spore_json_next(d, part)) {
        const spore_jnode *type = spore_json_get(d, part, "type");
        const spore_jnode *text = spore_json_get(d, part, "text");
        if (!type || type->type != SPORE_JSTR || strcmp(type->str, "text") ||
            !text || text->type != SPORE_JSTR) {
            spore_buf_free(&b);
            FAIL("only text content parts are supported");
        }
        if (b.len) spore_buf_add(&b, "\n", 1);
        spore_buf_add(&b, text->str, text->len);
    }
    spore_buf_add(&b, "", 0); /* ensure a NUL-terminated allocation */
    if (b.err || own(j, b.ptr)) {
        spore_buf_free(&b);
        FAIL("out of memory");
    }
    *out = b.ptr;
    return 0;
}

static int parse_chat(job *j, const spore_jnode *o, const char **err) {
    const spore_json *d = &j->doc;
    const spore_jnode *ms = spore_json_get(d, o, "messages");
    if (!ms || ms->type != SPORE_JARR || !ms->len)
        FAIL("'messages' must be a non-empty array");
    if (!(j->msgs = calloc(ms->len, sizeof *j->msgs))) FAIL("out of memory");
    size_t i = 0;
    for (const spore_jnode *m = spore_json_child(d, ms); m;
         m = spore_json_next(d, m), i++) {
        const spore_jnode *role = spore_json_get(d, m, "role");
        if (!role || role->type != SPORE_JSTR)
            FAIL("each message needs a string 'role'");
        j->msgs[i].role = role->str;
        if (content_text(j, spore_json_get(d, m, "content"),
                         &j->msgs[i].content, err))
            return -1;
    }
    j->p.messages = j->msgs;
    j->p.n_messages = ms->len;
    return parse_sampling(j, o, err);
}

static int parse_completion(job *j, const spore_jnode *o, const char **err) {
    const spore_json *d = &j->doc;
    const spore_jnode *pr = spore_json_get(d, o, "prompt");
    if (pr && pr->type == SPORE_JARR && pr->len == 1)
        pr = spore_json_child(d, pr);
    if (!pr || pr->type != SPORE_JSTR) FAIL("'prompt' must be a string");
    j->p.prompt = pr->str;
    return parse_sampling(j, o, err);
}

static int parse_embed(job *j, const spore_jnode *o, const char **err) {
    const spore_json *d = &j->doc;
    const spore_jnode *in = spore_json_get(d, o, "input");
    if (in && in->type == SPORE_JARR && in->len) {
        for (const spore_jnode *e = spore_json_child(d, in); e;
             e = spore_json_next(d, e))
            if (e->type != SPORE_JSTR)
                FAIL("'input' must be a string or an array of strings");
    } else if (!in || in->type != SPORE_JSTR) {
        FAIL("'input' must be a string or an array of strings");
    }
    j->input = in;
    const spore_jnode *f = spore_json_get(d, o, "encoding_format");
    if (f && f->type == SPORE_JSTR && strcmp(f->str, "base64") == 0)
        j->base64 = 1;
    else if (f && f->type != SPORE_JNULL &&
             !(f->type == SPORE_JSTR && strcmp(f->str, "float") == 0))
        FAIL("'encoding_format' must be \"float\" or \"base64\"");
    return 0;
}

/* ---- queue ------------------------------------------------------------ */

static void enqueue(spore_llm *llm, spore_req *req, spore_resp *resp,
                    int kind) {
    const char *err = "out of memory";
    int status = 500;
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->body = malloc(req->body_len + 1))) goto fail;
    memcpy(j->body, req->body, req->body_len);
    j->body[req->body_len] = '\0';
    j->kind = kind;
    j->resp = resp;
    j->created = (long)time(NULL);
    j->p = (spore_llm_params){.max_tokens = -1, .temperature = 0.8,
                              .top_p = 0.95, .top_k = 40, .min_p = 0.05,
                              .seed = SPORE_LLM_SEED_RANDOM,
                              .cache_prompt = 1};
    status = 400;
    if (spore_json_parse(&j->doc, j->body, req->body_len)) {
        err = "request body is not valid JSON";
        goto fail;
    }
    const spore_jnode *root = spore_json_root(&j->doc);
    if (root->type != SPORE_JOBJ) {
        err = "request body must be a JSON object";
        goto fail;
    }
    int rc = kind == JOB_CHAT         ? parse_chat(j, root, &err)
           : kind == JOB_COMPLETION ? parse_completion(j, root, &err)
                                    : parse_embed(j, root, &err);
    if (rc) goto fail;
    unsigned long n = atomic_fetch_add(&llm->seq, 1);
    snprintf(j->id, sizeof j->id, "%s-%lx%lx",
             kind == JOB_CHAT ? "chatcmpl" : "cmpl", (unsigned long)j->created,
             n);

    pthread_mutex_lock(&llm->mu);
    int ok = !atomic_load(&llm->shutdown) && llm->queued < llm->max_queue;
    if (ok) {
        if (llm->tail) llm->tail->next = j;
        else llm->head = j;
        llm->tail = j;
        llm->queued++;
        pthread_cond_signal(&llm->cv);
    }
    pthread_mutex_unlock(&llm->mu);
    if (ok) return;
    status = 503;
    err = "server busy";
fail:
    reply_error(resp, status, err);
    job_free(j);
}

/* ---- generation ------------------------------------------------------- */

typedef struct {
    spore_llm *llm;
    job *j;
    spore_buf acc; /* all generated text */
    size_t sent;   /* acc prefix already streamed, or checked for stops */
    int matched;   /* a stop sequence ended generation */
    int dead;      /* client gone or shutting down */
} gen;

static const char *find(const char *h, size_t hl, const char *n, size_t nl) {
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return h + i;
    return NULL;
}

static void chunk_head(gen *g, spore_buf *b) {
    job *j = g->j;
    spore_buf_printf(b, "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%ld,"
                        "\"model\":", j->id,
                     j->kind == JOB_CHAT ? "chat.completion.chunk"
                                         : "text_completion",
                     j->created);
    spore_json_str(b, g->llm->be.model, strlen(g->llm->be.model));
}

static void send_event(gen *g, spore_buf *b) {
    if (b->err || spore_sse(g->j->resp, NULL, b->ptr, b->len)) g->dead = 1;
    spore_buf_free(b);
}

/* One streamed delta, or the final chunk when finish != NULL. */
static void send_delta(gen *g, const char *text, size_t len,
                       const char *finish) {
    spore_buf b = {0};
    chunk_head(g, &b);
    spore_buf_puts(&b, ",\"choices\":[{\"index\":0,");
    if (g->j->kind == JOB_CHAT) {
        spore_buf_puts(&b, "\"delta\":{");
        if (text) {
            spore_buf_puts(&b, "\"content\":");
            spore_json_str(&b, text, len);
        }
        spore_buf_puts(&b, "}");
    } else {
        spore_buf_puts(&b, "\"text\":");
        spore_json_str(&b, text ? text : "", text ? len : 0);
        spore_buf_puts(&b, ",\"logprobs\":null");
    }
    if (finish) spore_buf_printf(&b, ",\"finish_reason\":\"%s\"}]}", finish);
    else spore_buf_puts(&b, ",\"finish_reason\":null}]}");
    send_event(g, &b);
}

static void advance(gen *g, size_t to) {
    if (to <= g->sent) return;
    if (g->j->stream) send_delta(g, g->acc.ptr + g->sent, to - g->sent, NULL);
    g->sent = to;
}

static int emit(void *ctx, const char *text, size_t len) {
    gen *g = ctx;
    if (g->matched || g->dead) return 1;
    if (atomic_load(&g->llm->shutdown)) {
        g->dead = 1;
        return 1;
    }
    spore_buf_add(&g->acc, text, len);
    if (g->acc.err) {
        g->dead = 1;
        return 1;
    }
    job *j = g->j;
    /* No stop match can start before `sent`; see the holdback below. */
    const char *first = NULL;
    for (size_t i = 0; i < j->n_stop; i++) {
        const char *m = find(g->acc.ptr + g->sent, g->acc.len - g->sent,
                             j->stop[i], strlen(j->stop[i]));
        if (m && (!first || m < first)) first = m;
    }
    if (first) {
        g->acc.len = (size_t)(first - g->acc.ptr);
        advance(g, g->acc.len);
        g->matched = 1;
        return 1;
    }
    /* Hold back the longest tail that could begin a stop sequence. */
    size_t hold = 0;
    for (size_t i = 0; i < j->n_stop; i++) {
        size_t sl = strlen(j->stop[i]);
        size_t max = sl - 1 < g->acc.len - g->sent ? sl - 1 : g->acc.len - g->sent;
        for (size_t h = max; h > hold; h--)
            if (memcmp(g->acc.ptr + g->acc.len - h, j->stop[i], h) == 0) {
                hold = h;
                break;
            }
    }
    advance(g, spore__utf8_cut(g->acc.ptr, g->sent, g->acc.len - hold));
    return g->dead;
}

static const char *finish_name(spore_llm_finish f, int matched) {
    return matched || f == SPORE_LLM_STOP ? "stop" : "length";
}

static void usage_json(spore_buf *b, const spore_llm_usage *u) {
    spore_buf_printf(b,
                     "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                     "\"total_tokens\":%d,"
                     "\"prompt_tokens_details\":{\"cached_tokens\":%d}}",
                     u->prompt_tokens, u->completion_tokens,
                     u->prompt_tokens + u->completion_tokens, u->cached_tokens);
}

static void run_generate(spore_llm *llm, job *j) {
    gen g = {llm, j, {0}, 0, 0, 0};
    spore_llm_usage usage = {0, 0, 0};
    if (j->stream) {
        spore_set_header(j->resp, "Cache-Control", "no-cache");
        if (spore_begin(j->resp, 200, "text/event-stream")) {
            spore_end(j->resp);
            return;
        }
        if (j->kind == JOB_CHAT) { /* OpenAI opens with the role alone */
            spore_buf b = {0};
            chunk_head(&g, &b);
            spore_buf_puts(&b, ",\"choices\":[{\"index\":0,\"delta\":"
                               "{\"role\":\"assistant\",\"content\":\"\"},"
                               "\"finish_reason\":null}]}");
            send_event(&g, &b);
        }
    }
    spore_llm_finish f = llm->be.generate(llm->be.self, &j->p, emit, &g, &usage);
    if (!g.matched && !g.dead) advance(&g, g.acc.len);

    if (j->stream) {
        if (g.dead) {
            /* client gone or shutting down: just close */
        } else if (f == SPORE_LLM_ERROR && !g.matched) {
            spore_buf b = {0};
            error_body(&b, "generation failed", 500);
            send_event(&g, &b);
        } else {
            send_delta(&g, NULL, 0, finish_name(f, g.matched));
            if (j->include_usage) {
                spore_buf b = {0};
                chunk_head(&g, &b);
                spore_buf_puts(&b, ",\"choices\":[],");
                usage_json(&b, &usage);
                spore_buf_puts(&b, "}");
                send_event(&g, &b);
            }
            spore_sse(j->resp, NULL, "[DONE]", 6);
        }
        spore_end(j->resp);
    } else if (f == SPORE_LLM_ERROR && !g.matched) {
        reply_error(j->resp, 500, "generation failed");
    } else if (g.dead || g.acc.err) {
        reply_error(j->resp, 500, "generation aborted");
    } else {
        spore_buf b = {0};
        spore_buf_printf(&b, "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%ld,"
                             "\"model\":",
                         j->id, j->kind == JOB_CHAT ? "chat.completion"
                                                    : "text_completion",
                         j->created);
        spore_json_str(&b, llm->be.model, strlen(llm->be.model));
        spore_buf_puts(&b, ",\"choices\":[{\"index\":0,");
        if (j->kind == JOB_CHAT) {
            spore_buf_puts(&b, "\"message\":{\"role\":\"assistant\",\"content\":");
            spore_json_str(&b, g.acc.ptr ? g.acc.ptr : "", g.acc.len);
            spore_buf_puts(&b, "}");
        } else {
            spore_buf_puts(&b, "\"text\":");
            spore_json_str(&b, g.acc.ptr ? g.acc.ptr : "", g.acc.len);
            spore_buf_puts(&b, ",\"logprobs\":null");
        }
        spore_buf_printf(&b, ",\"finish_reason\":\"%s\"}],",
                         finish_name(f, g.matched));
        usage_json(&b, &usage);
        spore_buf_puts(&b, "}");
        reply_json(j->resp, 200, &b);
    }
    spore_buf_free(&g.acc);
}

static void run_embed(spore_llm *llm, job *j) {
    const spore_json *d = &j->doc;
    size_t dim = llm->be.embed_dim;
    float *vec = malloc((dim ? dim : 1) * sizeof *vec);
    if (!vec) {
        reply_error(j->resp, 500, "out of memory");
        return;
    }
    spore_buf b = {0};
    spore_buf_puts(&b, "{\"object\":\"list\",\"data\":[");
    const spore_jnode *e = j->input->type == SPORE_JARR
                               ? spore_json_child(d, j->input) : j->input;
    long tokens = 0;
    for (size_t i = 0; e; i++, e = j->input->type == SPORE_JARR
                                       ? spore_json_next(d, e) : NULL) {
        int nt = 0;
        if (spore_closed(j->resp) || atomic_load(&llm->shutdown) ||
            llm->be.embed(llm->be.self, e->str, e->len, vec, &nt)) {
            free(vec);
            spore_buf_free(&b);
            reply_error(j->resp, 500, "embedding failed");
            return;
        }
        tokens += nt;
        spore_buf_printf(&b, "%s{\"object\":\"embedding\",\"index\":%zu,"
                             "\"embedding\":", i ? "," : "", i);
        if (j->base64) {
            /* little-endian float32, as the OpenAI API defines it */
            unsigned char *raw = malloc(dim * 4 + 1);
            if (!raw) {
                b.err = 1;
                break;
            }
            for (size_t k = 0; k < dim; k++) {
                uint32_t u;
                memcpy(&u, &vec[k], 4);
                for (int s = 0; s < 4; s++) raw[k * 4 + s] = (unsigned char)(u >> (8 * s));
            }
            spore_buf_puts(&b, "\"");
            spore__base64(&b, raw, dim * 4);
            spore_buf_puts(&b, "\"");
            free(raw);
        } else {
            spore_buf_puts(&b, "[");
            for (size_t k = 0; k < dim; k++) {
                if (k) spore_buf_puts(&b, ",");
                spore_json_num(&b, vec[k]);
            }
            spore_buf_puts(&b, "]");
        }
        spore_buf_puts(&b, "}");
    }
    free(vec);
    spore_buf_puts(&b, "],\"model\":");
    spore_json_str(&b, llm->be.model, strlen(llm->be.model));
    spore_buf_printf(&b, ",\"usage\":{\"prompt_tokens\":%ld,\"total_tokens\":%ld}}",
                     tokens, tokens);
    reply_json(j->resp, 200, &b);
}

static void *worker(void *arg) {
    spore_llm *llm = arg;
    for (;;) {
        pthread_mutex_lock(&llm->mu);
        while (!llm->head && !atomic_load(&llm->shutdown))
            pthread_cond_wait(&llm->cv, &llm->mu);
        job *j = llm->head;
        if (j) {
            llm->head = j->next;
            if (!llm->head) llm->tail = NULL;
            llm->queued--;
        }
        pthread_mutex_unlock(&llm->mu);
        if (!j) return NULL;
        if (atomic_load(&llm->shutdown)) reply_error(j->resp, 503, "shutting down");
        else if (spore_closed(j->resp)) spore_end(j->resp);
        else if (j->kind == JOB_EMBED) run_embed(llm, j);
        else run_generate(llm, j);
        job_free(j);
    }
}

/* ---- routes ----------------------------------------------------------- */

static void on_chat(spore_req *req, spore_resp *resp, void *ud) {
    enqueue(ud, req, resp, JOB_CHAT);
}

static void on_completion(spore_req *req, spore_resp *resp, void *ud) {
    enqueue(ud, req, resp, JOB_COMPLETION);
}

static void on_embed(spore_req *req, spore_resp *resp, void *ud) {
    enqueue(ud, req, resp, JOB_EMBED);
}

static void on_health(spore_req *req, spore_resp *resp, void *ud) {
    (void)req;
    (void)ud;
    static const char ok[] = "{\"status\":\"ok\"}";
    spore_reply(resp, 200, "application/json", ok, sizeof ok - 1);
}

static void on_models(spore_req *req, spore_resp *resp, void *ud) {
    (void)req;
    spore_llm *llm = ud;
    spore_buf b = {0};
    spore_buf_puts(&b, "{\"object\":\"list\",\"data\":[{\"id\":");
    spore_json_str(&b, llm->be.model, strlen(llm->be.model));
    spore_buf_puts(&b, ",\"object\":\"model\",\"created\":0,"
                       "\"owned_by\":\"spore\"}]}");
    reply_json(resp, 200, &b);
}

spore_llm *spore_llm_new(spore_server *srv, const spore_llm_backend *be,
                         const spore_llm_config *cfg) {
    if (!be->model || (!be->generate && !be->embed) ||
        (be->embed && !be->embed_dim))
        return NULL;
    spore_llm *llm = calloc(1, sizeof *llm);
    if (!llm) return NULL;
    llm->be = *be;
    llm->n_threads = cfg && cfg->workers ? cfg->workers : 1;
    llm->max_queue = cfg && cfg->queue ? cfg->queue : 16;
    pthread_mutex_init(&llm->mu, NULL);
    pthread_cond_init(&llm->cv, NULL);
    llm->threads = calloc(llm->n_threads, sizeof *llm->threads);
    if (!llm->threads) goto fail;
    if (spore_route(srv, "GET", "/health", on_health, llm) ||
        spore_route(srv, "GET", "/v1/models", on_models, llm) ||
        (be->generate &&
         (spore_route(srv, "POST", "/v1/chat/completions", on_chat, llm) ||
          spore_route(srv, "POST", "/v1/completions", on_completion, llm))) ||
        (be->embed && spore_route(srv, "POST", "/v1/embeddings", on_embed, llm)))
        goto fail;
    size_t started = 0;
    for (; started < llm->n_threads; started++)
        if (pthread_create(&llm->threads[started], NULL, worker, llm)) break;
    llm->n_threads = started;
    if (!started) goto fail;
    return llm;
fail:
    free(llm->threads);
    pthread_cond_destroy(&llm->cv);
    pthread_mutex_destroy(&llm->mu);
    free(llm);
    return NULL;
}

void spore_llm_free(spore_llm *llm) {
    if (!llm) return;
    pthread_mutex_lock(&llm->mu);
    atomic_store(&llm->shutdown, 1);
    pthread_cond_broadcast(&llm->cv);
    pthread_mutex_unlock(&llm->mu);
    for (size_t i = 0; i < llm->n_threads; i++)
        pthread_join(llm->threads[i], NULL);
    free(llm->threads);
    pthread_cond_destroy(&llm->cv);
    pthread_mutex_destroy(&llm->mu);
    free(llm);
}
