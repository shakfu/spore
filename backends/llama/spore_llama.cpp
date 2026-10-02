// llama.cpp backend for spore_llm.
// SPDX-License-Identifier: MIT
//
// One context, serialised by a mutex, with no batching across requests.
// The KV cache persists between requests (prompt caching). Each cache slot
// is a llama.cpp sequence in one unified KV buffer, so interleaved
// conversations keep their own prefixes, and a prefix shared between
// slots (a common system prompt) occupies its cells once.
#include "spore_llama.h"

#include "llama.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct slot {
    std::vector<llama_token> cached; // tokens whose KV entries are in this seq
    uint64_t used = 0;               // last use, for LRU
};

struct backend {
    spore_llm_backend be; // the C API hands out &be; be.self leads back
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    std::string name;
    std::mutex mu;
    std::vector<slot> slots; // index = llama seq id
    uint64_t clock = 0;
    llama_batch batch{};
    int n_batch = 0;
};

void log_warnings(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

bool tokenize(const llama_vocab *vocab, const std::string &text, bool special,
              std::vector<llama_token> &out) {
    int n = -llama_tokenize(vocab, text.data(), (int32_t)text.size(), nullptr,
                            0, true, special);
    if (n <= 0) return false;
    out.resize((size_t)n);
    return llama_tokenize(vocab, text.data(), (int32_t)text.size(), out.data(),
                          n, true, special) == n;
}

bool apply_template(backend *b, const spore_llm_params *p, std::string &out) {
    std::vector<llama_chat_message> msgs;
    size_t total = 0;
    for (size_t i = 0; i < p->n_messages; i++) {
        msgs.push_back({p->messages[i].role, p->messages[i].content});
        total += strlen(p->messages[i].content) + 64;
    }
    const char *tmpl = llama_model_chat_template(b->model, nullptr);
    std::vector<char> buf(total * 2 + 256);
    int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true,
                                      buf.data(), (int32_t)buf.size());
    if (n > (int)buf.size()) {
        buf.resize((size_t)n);
        n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true,
                                      buf.data(), (int32_t)buf.size());
    }
    if (n < 0) return false;
    out.assign(buf.data(), (size_t)n);
    return true;
}

// The prompt ends inside a <think> block that the template opened.
bool opens_think(const std::string &prompt) {
    size_t n = prompt.find_last_not_of(" \t\r\n");
    return n != std::string::npos && n >= 6 && prompt.compare(n - 6, 7, "<think>") == 0;
}

llama_sampler *make_sampler(const spore_llm_params *p) {
    llama_sampler *s =
        llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (p->temperature <= 0) {
        llama_sampler_chain_add(s, llama_sampler_init_greedy());
        return s;
    }
    if (p->top_k > 0) llama_sampler_chain_add(s, llama_sampler_init_top_k(p->top_k));
    llama_sampler_chain_add(s, llama_sampler_init_top_p((float)p->top_p, 1));
    llama_sampler_chain_add(s, llama_sampler_init_min_p((float)p->min_p, 1));
    llama_sampler_chain_add(s, llama_sampler_init_temp((float)p->temperature));
    llama_sampler_chain_add(s, llama_sampler_init_dist(p->seed)); // same "random" sentinel
    return s;
}

spore_llm_finish error(spore_llm_usage *u, spore_llm_finish f, const char *msg) {
    snprintf(u->error, sizeof u->error, "%s", msg);
    return f;
}

// A failed decode leaves the KV state unknown: forget it.
spore_llm_finish fail(backend *b, spore_llm_usage *u, int rc) {
    llama_memory_clear(llama_get_memory(b->ctx), true);
    for (slot &s : b->slots) s.cached.clear();
    snprintf(u->error, sizeof u->error,
             rc == 1 ? "the KV cache is full" : "llama_decode failed (%d)", rc);
    return SPORE_LLM_ERROR;
}

// Decode n tokens of `seq` from position `pos`; logits for the last only.
// When the KV buffer is full, evict other slots, least recently used first.
int decode(backend *b, int seq, const llama_token *t, int n, int pos) {
    llama_batch &bt = b->batch;
    for (int i = 0; i < n; i++) {
        bt.token[i] = t[i];
        bt.pos[i] = pos + i;
        bt.n_seq_id[i] = 1;
        bt.seq_id[i][0] = seq;
        bt.logits[i] = i == n - 1;
    }
    bt.n_tokens = n;
    for (;;) {
        int rc = llama_decode(b->ctx, bt);
        if (rc != 1) return rc; // 1: no room; the KV state is unchanged
        int victim = -1;
        for (int i = 0; i < (int)b->slots.size(); i++)
            if (i != seq && !b->slots[(size_t)i].cached.empty() &&
                (victim < 0 || b->slots[(size_t)i].used < b->slots[(size_t)victim].used))
                victim = i;
        if (victim < 0) return rc;
        llama_memory_seq_rm(llama_get_memory(b->ctx), victim, -1, -1);
        b->slots[(size_t)victim].cached.clear();
    }
}

// Pick the slot for `toks` and prepare its KV state. Returns the slot and
// sets `keep` to the prefix length already evaluated in it. The slot with
// the longest shared prefix is extended in place when that discards at most
// one token; otherwise the prefix is copied into the least recently used
// slot, so a conversation that only shares a system prompt is not evicted.
int prepare_slot(backend *b, const std::vector<llama_token> &toks, bool reuse,
                 size_t &keep) {
    size_t best = 0, lru = 0;
    keep = 0;
    for (size_t i = 0; i < b->slots.size(); i++) {
        const std::vector<llama_token> &c = b->slots[i].cached;
        size_t k = 0;
        while (reuse && k < c.size() && k < toks.size() && c[k] == toks[k]) k++;
        if (k > keep) keep = k, best = i;
        if (b->slots[i].used < b->slots[lru].used) lru = i;
    }
    // Always re-decode at least one prompt token: sampling needs its logits.
    if (keep == toks.size()) keep--;
    llama_memory_t mem = llama_get_memory(b->ctx);
    size_t seq = keep && b->slots[best].cached.size() <= keep + 1 ? best : lru;
    if (seq != best) {
        llama_memory_seq_rm(mem, (llama_seq_id)seq, -1, -1);
        if (keep) llama_memory_seq_cp(mem, (llama_seq_id)best, (llama_seq_id)seq, 0,
                                      (llama_pos)keep);
        b->slots[seq].cached.assign(b->slots[best].cached.begin(),
                                    b->slots[best].cached.begin() + (long)keep);
    }
    if (!llama_memory_seq_rm(mem, (llama_seq_id)seq, (llama_pos)keep, -1)) {
        llama_memory_seq_rm(mem, (llama_seq_id)seq, -1, -1); // recurrent: no partial removal
        keep = 0;
    }
    b->slots[seq].cached.resize(keep);
    b->slots[seq].used = ++b->clock;
    return (int)seq;
}

spore_llm_finish run(backend *b, const spore_llm_params *p, spore_llm_emit emit,
                     void *ectx, spore_llm_usage *usage) {
    std::string prompt;
    if (p->messages) {
        if (!apply_template(b, p, prompt))
            return error(usage, SPORE_LLM_ERROR, "the model's chat template failed");
    } else {
        prompt = p->prompt;
    }
    std::vector<llama_token> toks;
    if (!tokenize(b->vocab, prompt, true, toks))
        return error(usage, SPORE_LLM_ERROR, "tokenization failed");
    const int n_ctx = (int)llama_n_ctx(b->ctx);
    const int n_prompt = (int)toks.size();
    usage->prompt_tokens = n_prompt;
    if (n_prompt >= n_ctx) {
        snprintf(usage->error, sizeof usage->error,
                 "the prompt has %d tokens; the context holds %d", n_prompt, n_ctx);
        return SPORE_LLM_INVALID;
    }

    size_t keep;
    const int seq = prepare_slot(b, toks, p->cache_prompt, keep);
    std::vector<llama_token> &cached = b->slots[(size_t)seq].cached;
    usage->cached_tokens = (int)keep;

    // Batches of at most 256, polling between them (emit with no text), so
    // a cancel or barge-in need not wait for a long uncached prompt.
    for (int i = (int)keep; i < n_prompt; i += b->n_batch) {
        if (i > (int)keep && emit(ectx, "", 0)) return SPORE_LLM_STOP;
        int n = n_prompt - i < b->n_batch ? n_prompt - i : b->n_batch;
        if (int rc = decode(b, seq, toks.data() + i, n, i)) return fail(b, usage, rc);
        cached.insert(cached.end(), toks.begin() + i, toks.begin() + i + n);
    }

    // Restore the tag, so the reply can be split like one that opens it.
    if (p->messages && opens_think(prompt) && emit(ectx, "<think>", 7))
        return SPORE_LLM_STOP;
    llama_sampler *smpl = make_sampler(p);
    spore_llm_finish fin = SPORE_LLM_STOP;
    std::vector<char> piece(256);
    for (int n_gen = 0;; n_gen++) {
        if ((p->max_tokens >= 0 && n_gen >= p->max_tokens) ||
            n_prompt + n_gen >= n_ctx) {
            fin = SPORE_LLM_LENGTH;
            break;
        }
        llama_token tok = llama_sampler_sample(smpl, b->ctx, -1);
        if (llama_vocab_is_eog(b->vocab, tok)) break;
        int n = llama_token_to_piece(b->vocab, tok, piece.data(),
                                     (int32_t)piece.size(), 0, false);
        if (n < 0) {
            piece.resize((size_t)-n);
            n = llama_token_to_piece(b->vocab, tok, piece.data(),
                                     (int32_t)piece.size(), 0, false);
        }
        usage->completion_tokens = n_gen + 1;
        if (n > 0 && emit(ectx, piece.data(), (size_t)n)) break;
        if (int rc = decode(b, seq, &tok, 1, n_prompt + n_gen)) {
            fin = fail(b, usage, rc);
            break;
        }
        cached.push_back(tok);
    }
    llama_sampler_free(smpl);
    return fin;
}

spore_llm_finish generate(void *self, const spore_llm_params *p,
                          spore_llm_emit emit, void *ectx,
                          spore_llm_usage *usage) {
    backend *b = static_cast<backend *>(self);
    std::lock_guard<std::mutex> lock(b->mu);
    try {
        return run(b, p, emit, ectx, usage);
    } catch (...) {
        return error(usage, SPORE_LLM_ERROR, "internal error");
    }
}

spore_llm_finish embed(void *self, const char *text, size_t len, float *out,
                       spore_llm_usage *usage) {
    backend *b = static_cast<backend *>(self);
    std::lock_guard<std::mutex> lock(b->mu);
    try {
        std::vector<llama_token> toks;
        if (!tokenize(b->vocab, std::string(text, len), false, toks))
            return error(usage, SPORE_LLM_ERROR, "tokenization failed");
        const int n = (int)toks.size();
        const int n_batch = (int)llama_n_batch(b->ctx);
        usage->prompt_tokens = n;
        if (n > n_batch) {
            snprintf(usage->error, sizeof usage->error,
                     "the input has %d tokens; the limit is %d", n, n_batch);
            return SPORE_LLM_INVALID;
        }
        llama_memory_clear(llama_get_memory(b->ctx), true);
        llama_batch batch = llama_batch_init(n, 0, 1);
        for (int i = 0; i < n; i++) {
            batch.token[i] = toks[(size_t)i];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = 1;
        }
        batch.n_tokens = n;
        int rc = llama_model_has_encoder(b->model) && !llama_model_has_decoder(b->model)
                     ? llama_encode(b->ctx, batch)
                     : llama_decode(b->ctx, batch);
        llama_batch_free(batch);
        if (rc) {
            snprintf(usage->error, sizeof usage->error, "llama_decode failed (%d)", rc);
            return SPORE_LLM_ERROR;
        }
        const float *e = llama_get_embeddings_seq(b->ctx, 0);
        if (!e) return error(usage, SPORE_LLM_ERROR, "the model returned no embedding");
        // L2-normalise, as the OpenAI endpoint and llama-server do.
        double sum = 0;
        for (size_t i = 0; i < b->be.embed_dim; i++) sum += (double)e[i] * e[i];
        const float scale = sum > 0 ? (float)(1.0 / std::sqrt(sum)) : 0.0f;
        for (size_t i = 0; i < b->be.embed_dim; i++) out[i] = e[i] * scale;
        return SPORE_LLM_STOP;
    } catch (...) {
        return error(usage, SPORE_LLM_ERROR, "internal error");
    }
}

} // namespace

extern "C" spore_llm_backend *spore_llama_new(const spore_llama_config *cfg) {
    backend *b = nullptr;
    try {
        b = new backend();
        b->be.self = b;
        llama_log_set(log_warnings, nullptr);
        llama_backend_init();
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = cfg->n_gpu_layers;
        b->model = llama_model_load_from_file(cfg->model_path, mp);
        if (!b->model) throw 0;
        b->vocab = llama_model_get_vocab(b->model);

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = (uint32_t)cfg->n_ctx;
        if (cfg->n_threads > 0) cp.n_threads = cp.n_threads_batch = cfg->n_threads;
        int n_slots = cfg->embedding ? 1 : cfg->n_slots > 0 ? cfg->n_slots : 1;
        // Every slot may use the whole context, sharing cells for common prefixes.
        cp.n_seq_max = (uint32_t)std::min(n_slots, 64);
        cp.kv_unified = true;
        if (cfg->embedding) {
            cp.embeddings = true;
            // Non-causal models need the whole input in one micro-batch.
            cp.n_batch = cp.n_ubatch = cp.n_ctx ? cp.n_ctx : 512;
        }
        b->ctx = llama_init_from_model(b->model, cp);
        if (!b->ctx) throw 0;
        b->slots.resize(cp.n_seq_max);
        b->n_batch = std::min<int>((int)llama_n_batch(b->ctx), 256);
        b->batch = llama_batch_init(b->n_batch, 0, 1);
        if (cfg->embedding && llama_pooling_type(b->ctx) == LLAMA_POOLING_TYPE_NONE) {
            fprintf(stderr, "spore_llama: model has no pooling; cannot embed\n");
            throw 0;
        }

        const char *slash = strrchr(cfg->model_path, '/');
        b->name = slash ? slash + 1 : cfg->model_path;
        b->be.model = b->name.c_str();
        if (cfg->embedding) {
            b->be.embed = embed;
            b->be.embed_dim = (size_t)llama_model_n_embd(b->model);
        } else {
            b->be.generate = generate;
        }
        return &b->be;
    } catch (...) {
        spore_llama_free(b ? &b->be : nullptr);
        return nullptr;
    }
}

extern "C" void spore_llama_free(spore_llm_backend *be) {
    if (!be) return;
    backend *b = static_cast<backend *>(be->self);
    if (b->batch.token) llama_batch_free(b->batch);
    if (b->ctx) llama_free(b->ctx);
    if (b->model) llama_model_free(b->model);
    delete b;
}
