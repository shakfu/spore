// llama.cpp backend for spore_llm.
// SPDX-License-Identifier: MIT
//
// One context, serialised by a mutex, with no batching across requests.
// The KV cache persists between requests: each request reuses the longest
// token prefix it shares with the previous one (prompt caching).
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

struct backend {
    spore_llm_backend be; // the C API hands out &be; be.self leads back
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    std::string name;
    std::mutex mu;
    std::vector<llama_token> cached; // tokens whose KV entries are in seq 0
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

// A failed decode leaves the KV state unknown: forget it.
spore_llm_finish fail(backend *b) {
    llama_memory_clear(llama_get_memory(b->ctx), true);
    b->cached.clear();
    return SPORE_LLM_ERROR;
}

spore_llm_finish run(backend *b, const spore_llm_params *p, spore_llm_emit emit,
                     void *ectx, spore_llm_usage *usage) {
    std::string prompt;
    if (p->messages) {
        if (!apply_template(b, p, prompt)) return SPORE_LLM_ERROR;
    } else {
        prompt = p->prompt;
    }
    std::vector<llama_token> toks;
    if (!tokenize(b->vocab, prompt, true, toks)) return SPORE_LLM_ERROR;
    const int n_ctx = (int)llama_n_ctx(b->ctx);
    const int n_prompt = (int)toks.size();
    usage->prompt_tokens = n_prompt;
    if (n_prompt >= n_ctx) return SPORE_LLM_ERROR;

    // Reuse the shared prefix. Always re-decode at least one prompt token:
    // sampling needs the logits of the last one.
    size_t keep = 0;
    while (p->cache_prompt && keep < b->cached.size() && keep < toks.size() &&
           b->cached[keep] == toks[keep])
        keep++;
    if (keep == toks.size()) keep--;
    llama_memory_t mem = llama_get_memory(b->ctx);
    if (!llama_memory_seq_rm(mem, 0, (llama_pos)keep, -1)) {
        llama_memory_clear(mem, true); // recurrent/SWA caches: no partial removal
        keep = 0;
    }
    b->cached.resize(keep);
    usage->cached_tokens = (int)keep;

    // Batches of at most 256, polling between them (emit with no text), so
    // a cancel or barge-in need not wait for a long uncached prompt.
    const int n_batch = std::min<int>((int)llama_n_batch(b->ctx), 256);
    for (int i = (int)keep; i < n_prompt; i += n_batch) {
        if (i > (int)keep && emit(ectx, "", 0)) return SPORE_LLM_STOP;
        int n = n_prompt - i < n_batch ? n_prompt - i : n_batch;
        if (llama_decode(b->ctx, llama_batch_get_one(toks.data() + i, n)))
            return fail(b);
        b->cached.insert(b->cached.end(), toks.begin() + i, toks.begin() + i + n);
    }

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
        if (llama_decode(b->ctx, llama_batch_get_one(&tok, 1))) {
            fin = fail(b);
            break;
        }
        b->cached.push_back(tok);
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
        return SPORE_LLM_ERROR;
    }
}

int embed(void *self, const char *text, size_t len, float *out,
          int *n_tokens) {
    backend *b = static_cast<backend *>(self);
    std::lock_guard<std::mutex> lock(b->mu);
    try {
        std::vector<llama_token> toks;
        if (!tokenize(b->vocab, std::string(text, len), false, toks)) return -1;
        const int n = (int)toks.size();
        if (n > (int)llama_n_batch(b->ctx)) return -1;
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
        if (rc) return -1;
        const float *e = llama_get_embeddings_seq(b->ctx, 0);
        if (!e) return -1;
        // L2-normalise, as the OpenAI endpoint and llama-server do.
        double sum = 0;
        for (size_t i = 0; i < b->be.embed_dim; i++) sum += (double)e[i] * e[i];
        const float scale = sum > 0 ? (float)(1.0 / std::sqrt(sum)) : 0.0f;
        for (size_t i = 0; i < b->be.embed_dim; i++) out[i] = e[i] * scale;
        *n_tokens = n;
        return 0;
    } catch (...) {
        return -1;
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
        if (cfg->embedding) {
            cp.embeddings = true;
            // Non-causal models need the whole input in one micro-batch.
            cp.n_batch = cp.n_ubatch = cp.n_ctx ? cp.n_ctx : 512;
        }
        b->ctx = llama_init_from_model(b->model, cp);
        if (!b->ctx) throw 0;
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
    if (b->ctx) llama_free(b->ctx);
    if (b->model) llama_model_free(b->model);
    delete b;
}
