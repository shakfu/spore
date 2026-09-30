// OuteTTS text-to-speech adapter: text -> audio codes (OuteTTS LLM) ->
// spectrum (WavTokenizer) -> 24 kHz audio (inverse DFT + overlap-add).
// SPDX-License-Identifier: MIT
//
// Ported from cyllama's TTS (MIT), itself derived from llama.cpp's OuteTTS
// example (MIT). The default speaker profile is OuteTTS's en_male_1.
#include "spore_outetts.h"

#include "llama.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int N_CODES = 4096; // WavTokenizer codebook; code i is token <|i|>
constexpr int N_FFT = 1280, N_HOP = 320, N_WIN = 1280;
constexpr int N_PAD = (N_WIN - N_HOP) / 2;
constexpr double PI = 3.14159265358979323846;

// OuteTTS 0.2 layout; 0.3 drops <|code_start|> and ends words with <|space|>.
const char *const SPEAKER_WORDS =
    "the overall package from just two people is pretty remarkable sure i "
    "have some critiques about some of the gameplay aspects but its still "
    "really enjoyable and it looks lovely";
const char *const SPEAKER_CODES = R"SPK(<|audio_start|>
the<|t_0.08|><|code_start|><|257|><|740|><|636|><|913|><|788|><|1703|><|code_end|>
overall<|t_0.36|><|code_start|><|127|><|201|><|191|><|774|><|700|><|532|><|1056|><|557|><|798|><|298|><|1741|><|747|><|1662|><|1617|><|1702|><|1527|><|368|><|1588|><|1049|><|1008|><|1625|><|747|><|1576|><|728|><|1019|><|1696|><|1765|><|code_end|>
package<|t_0.56|><|code_start|><|935|><|584|><|1319|><|627|><|1016|><|1491|><|1344|><|1117|><|1526|><|1040|><|239|><|1435|><|951|><|498|><|723|><|1180|><|535|><|789|><|1649|><|1637|><|78|><|465|><|1668|><|901|><|595|><|1675|><|117|><|1009|><|1667|><|320|><|840|><|79|><|507|><|1762|><|1508|><|1228|><|1768|><|802|><|1450|><|1457|><|232|><|639|><|code_end|>
from<|t_0.19|><|code_start|><|604|><|782|><|1682|><|872|><|1532|><|1600|><|1036|><|1761|><|647|><|1554|><|1371|><|653|><|1595|><|950|><|code_end|>
just<|t_0.25|><|code_start|><|1782|><|1670|><|317|><|786|><|1748|><|631|><|599|><|1155|><|1364|><|1524|><|36|><|1591|><|889|><|1535|><|541|><|440|><|1532|><|50|><|870|><|code_end|>
two<|t_0.24|><|code_start|><|1681|><|1510|><|673|><|799|><|805|><|1342|><|330|><|519|><|62|><|640|><|1138|><|565|><|1552|><|1497|><|1552|><|572|><|1715|><|1732|><|code_end|>
people<|t_0.39|><|code_start|><|593|><|274|><|136|><|740|><|691|><|633|><|1484|><|1061|><|1138|><|1485|><|344|><|428|><|397|><|1562|><|645|><|917|><|1035|><|1449|><|1669|><|487|><|442|><|1484|><|1329|><|1832|><|1704|><|600|><|761|><|653|><|269|><|code_end|>
is<|t_0.16|><|code_start|><|566|><|583|><|1755|><|646|><|1337|><|709|><|802|><|1008|><|485|><|1583|><|652|><|10|><|code_end|>
pretty<|t_0.32|><|code_start|><|1818|><|1747|><|692|><|733|><|1010|><|534|><|406|><|1697|><|1053|><|1521|><|1355|><|1274|><|816|><|1398|><|211|><|1218|><|817|><|1472|><|1703|><|686|><|13|><|822|><|445|><|1068|><|code_end|>
remarkable<|t_0.68|><|code_start|><|230|><|1048|><|1705|><|355|><|706|><|1149|><|1535|><|1787|><|1356|><|1396|><|835|><|1583|><|486|><|1249|><|286|><|937|><|1076|><|1150|><|614|><|42|><|1058|><|705|><|681|><|798|><|934|><|490|><|514|><|1399|><|572|><|1446|><|1703|><|1346|><|1040|><|1426|><|1304|><|664|><|171|><|1530|><|625|><|64|><|1708|><|1830|><|1030|><|443|><|1509|><|1063|><|1605|><|1785|><|721|><|1440|><|923|><|code_end|>
sure<|t_0.36|><|code_start|><|792|><|1780|><|923|><|1640|><|265|><|261|><|1525|><|567|><|1491|><|1250|><|1730|><|362|><|919|><|1766|><|543|><|1|><|333|><|113|><|970|><|252|><|1606|><|133|><|302|><|1810|><|1046|><|1190|><|1675|><|code_end|>
i<|t_0.08|><|code_start|><|123|><|439|><|1074|><|705|><|1799|><|637|><|code_end|>
have<|t_0.16|><|code_start|><|1509|><|599|><|518|><|1170|><|552|><|1029|><|1267|><|864|><|419|><|143|><|1061|><|0|><|code_end|>
some<|t_0.16|><|code_start|><|619|><|400|><|1270|><|62|><|1370|><|1832|><|917|><|1661|><|167|><|269|><|1366|><|1508|><|code_end|>
critiques<|t_0.60|><|code_start|><|559|><|584|><|1163|><|1129|><|1313|><|1728|><|721|><|1146|><|1093|><|577|><|928|><|27|><|630|><|1080|><|1346|><|1337|><|320|><|1382|><|1175|><|1682|><|1556|><|990|><|1683|><|860|><|1721|><|110|><|786|><|376|><|1085|><|756|><|1523|><|234|><|1334|><|1506|><|1578|><|659|><|612|><|1108|><|1466|><|1647|><|308|><|1470|><|746|><|556|><|1061|><|code_end|>
about<|t_0.29|><|code_start|><|26|><|1649|><|545|><|1367|><|1263|><|1728|><|450|><|859|><|1434|><|497|><|1220|><|1285|><|179|><|755|><|1154|><|779|><|179|><|1229|><|1213|><|922|><|1774|><|1408|><|code_end|>
some<|t_0.23|><|code_start|><|986|><|28|><|1649|><|778|><|858|><|1519|><|1|><|18|><|26|><|1042|><|1174|><|1309|><|1499|><|1712|><|1692|><|1516|><|1574|><|code_end|>
of<|t_0.07|><|code_start|><|197|><|716|><|1039|><|1662|><|64|><|code_end|>
the<|t_0.08|><|code_start|><|1811|><|1568|><|569|><|886|><|1025|><|1374|><|code_end|>
gameplay<|t_0.48|><|code_start|><|1269|><|1092|><|933|><|1362|><|1762|><|1700|><|1675|><|215|><|781|><|1086|><|461|><|838|><|1022|><|759|><|649|><|1416|><|1004|><|551|><|909|><|787|><|343|><|830|><|1391|><|1040|><|1622|><|1779|><|1360|><|1231|><|1187|><|1317|><|76|><|997|><|989|><|978|><|737|><|189|><|code_end|>
aspects<|t_0.56|><|code_start|><|1423|><|797|><|1316|><|1222|><|147|><|719|><|1347|><|386|><|1390|><|1558|><|154|><|440|><|634|><|592|><|1097|><|1718|><|712|><|763|><|1118|><|1721|><|1311|><|868|><|580|><|362|><|1435|><|868|><|247|><|221|><|886|><|1145|><|1274|><|1284|><|457|><|1043|><|1459|><|1818|><|62|><|599|><|1035|><|62|><|1649|><|778|><|code_end|>
but<|t_0.20|><|code_start|><|780|><|1825|><|1681|><|1007|><|861|><|710|><|702|><|939|><|1669|><|1491|><|613|><|1739|><|823|><|1469|><|648|><|code_end|>
its<|t_0.09|><|code_start|><|92|><|688|><|1623|><|962|><|1670|><|527|><|599|><|code_end|>
still<|t_0.27|><|code_start|><|636|><|10|><|1217|><|344|><|713|><|957|><|823|><|154|><|1649|><|1286|><|508|><|214|><|1760|><|1250|><|456|><|1352|><|1368|><|921|><|615|><|5|><|code_end|>
really<|t_0.36|><|code_start|><|55|><|420|><|1008|><|1659|><|27|><|644|><|1266|><|617|><|761|><|1712|><|109|><|1465|><|1587|><|503|><|1541|><|619|><|197|><|1019|><|817|><|269|><|377|><|362|><|1381|><|507|><|1488|><|4|><|1695|><|code_end|>
enjoyable<|t_0.49|><|code_start|><|678|><|501|><|864|><|319|><|288|><|1472|><|1341|><|686|><|562|><|1463|><|619|><|1563|><|471|><|911|><|730|><|1811|><|1006|><|520|><|861|><|1274|><|125|><|1431|><|638|><|621|><|153|><|876|><|1770|><|437|><|987|><|1653|><|1109|><|898|><|1285|><|80|><|593|><|1709|><|843|><|code_end|>
and<|t_0.15|><|code_start|><|1285|><|987|><|303|><|1037|><|730|><|1164|><|502|><|120|><|1737|><|1655|><|1318|><|code_end|>
it<|t_0.09|><|code_start|><|848|><|1366|><|395|><|1601|><|1513|><|593|><|1302|><|code_end|>
looks<|t_0.27|><|code_start|><|1281|><|1266|><|1755|><|572|><|248|><|1751|><|1257|><|695|><|1380|><|457|><|659|><|585|><|1315|><|1105|><|1776|><|736|><|24|><|736|><|654|><|1027|><|code_end|>
lovely<|t_0.56|><|code_start|><|634|><|596|><|1766|><|1556|><|1306|><|1285|><|1481|><|1721|><|1123|><|438|><|1246|><|1251|><|795|><|659|><|1381|><|1658|><|217|><|1772|><|562|><|952|><|107|><|1129|><|1112|><|467|><|550|><|1079|><|840|><|1615|><|1469|><|1380|><|168|><|917|><|836|><|1827|><|437|><|583|><|67|><|595|><|1087|><|1646|><|1493|><|1677|><|code_end|>
)SPK";

struct tts {
    llama_model *ttc = nullptr, *cts = nullptr;
    llama_context *cttc = nullptr, *ccts = nullptr;
    const llama_vocab *vocab = nullptr;
    bool v03 = false;
    llama_token newline = -1, code_lo = -1, code_hi = -1;
    std::string sep, audio_text, audio_data;
    int n_embd = 0, n_threads = 8;
    size_t first_chunk = 0, chunk = 0, left = 0, ahead = 0; // streaming, in codes
    std::vector<float> cos_t, sin_t, hann;
    std::mutex mu;
};

void log_warnings(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

std::vector<llama_token> tokenize(const llama_vocab *v, const std::string &s,
                                  bool add_special) {
    int n = -llama_tokenize(v, s.data(), (int32_t)s.size(), nullptr, 0,
                            add_special, true);
    std::vector<llama_token> out(n > 0 ? (size_t)n : 0);
    if (n > 0 && llama_tokenize(v, s.data(), (int32_t)s.size(), out.data(), n,
                                add_special, true) != n)
        out.clear();
    return out;
}

llama_token single_token(const llama_vocab *v, const std::string &s) {
    auto t = tokenize(v, s, false);
    return t.size() == 1 ? t[0] : -1;
}

// ---- text normalisation (English) -----------------------------------------

const char *const ONES[] = {"zero", "one", "two", "three", "four", "five", "six",
                            "seven", "eight", "nine", "ten", "eleven", "twelve",
                            "thirteen", "fourteen", "fifteen", "sixteen",
                            "seventeen", "eighteen", "nineteen"};
const char *const TENS[] = {"", "", "twenty", "thirty", "forty", "fifty",
                            "sixty", "seventy", "eighty", "ninety"};

void below_thousand(int n, std::string &out) {
    if (n >= 100) {
        out += ONES[n / 100];
        out += " hundred ";
        n %= 100;
    }
    if (n >= 20) {
        out += TENS[n / 10];
        if (n % 10) {
            out += ' ';
            out += ONES[n % 10];
        }
    } else if (n > 0) {
        out += ONES[n];
    }
    out += ' ';
}

void number_words(const std::string &digits, std::string &out) {
    if (digits.size() > 12) { // too large to read as a number: digit by digit
        for (char c : digits) {
            out += ONES[c - '0'];
            out += ' ';
        }
        return;
    }
    long long n = std::stoll(digits);
    if (n == 0) {
        out += "zero ";
        return;
    }
    static const struct { long long unit; const char *name; } scale[] = {
        {1000000000LL, "billion"}, {1000000, "million"}, {1000, "thousand"}};
    for (auto &s : scale)
        if (n >= s.unit) {
            below_thousand((int)(n / s.unit), out);
            out += s.name;
            out += ' ';
            n %= s.unit;
        }
    if (n) below_thousand((int)n, out);
}

// Numbers become words, then only lowercase a-z words remain, joined by sep.
std::string normalise(const char *text, size_t len, const std::string &sep) {
    std::string spelled;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)text[i];
        if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < len && text[j] >= '0' && text[j] <= '9') j++;
            spelled += ' ';
            number_words(std::string(text + i, j - i), spelled);
            if (j + 1 < len && text[j] == '.' && text[j + 1] >= '0' && text[j + 1] <= '9') {
                spelled += "point ";
                for (j++; j < len && text[j] >= '0' && text[j] <= '9'; j++) {
                    spelled += ONES[text[j] - '0'];
                    spelled += ' ';
                }
            }
            i = j;
        } else {
            spelled += (char)c;
            i++;
        }
    }
    std::string out;
    bool gap = false;
    for (unsigned char c : spelled) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c >= 'a' && c <= 'z') {
            if (gap && !out.empty()) out += sep;
            gap = false;
            out += (char)c;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                   strchr("-_/,.\\", c)) {
            gap = true;
        } // anything else (apostrophes, non-ASCII) is dropped
    }
    return out;
}

// ---- vocoder output to audio ----------------------------------------------

// Frame spectrum -> time domain, matching the reference: the real part of
// sum_m X[m] e^{2 pi i k m / n}, over the n/2+1 stored bins, scaled by 1/N.
void inverse_dft(const tts *t, const float *cplx, float *out) {
    const int N = N_FFT / 2 + 1;
    for (int k = 0; k < N_FFT; k++) {
        double acc = 0;
        int idx = 0; // (k * m) mod N_FFT, advanced incrementally
        for (int m = 0; m < N; m++) {
            acc += cplx[2 * m] * t->cos_t[idx] - cplx[2 * m + 1] * t->sin_t[idx];
            idx += k;
            if (idx >= N_FFT) idx -= N_FFT;
        }
        out[k] = (float)(acc / N);
    }
}

std::vector<float> embd_to_audio(const tts *t, const std::vector<float> &embd,
                                 int n_codes) {
    const int n_embd = t->n_embd, half = n_embd / 2;
    const int n_out = (n_codes - 1) * N_HOP + N_WIN;
    std::vector<float> frames((size_t)n_codes * N_FFT);
    auto work = [&](int first) {
        std::vector<float> spec(n_embd);
        for (int l = first; l < n_codes; l += t->n_threads) {
            const float *e = embd.data() + (size_t)l * n_embd;
            for (int k = 0; k < half; k++) { // log-magnitude, then phase
                float mag = std::min(std::exp(e[k]), 1e2f);
                spec[2 * k] = mag * std::cos(e[k + half]);
                spec[2 * k + 1] = mag * std::sin(e[k + half]);
            }
            float *f = frames.data() + (size_t)l * N_FFT;
            inverse_dft(t, spec.data(), f);
            for (int j = 0; j < N_FFT; j++) f[j] *= t->hann[j];
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < t->n_threads; i++) pool.emplace_back(work, i);
    work(0);
    for (auto &th : pool) th.join();

    // Overlap-add, then divide by the summed squared window.
    std::vector<float> audio(n_out, 0.0f), env(n_out, 0.0f);
    for (int l = 0; l < n_codes; l++)
        for (int j = 0; j < N_WIN; j++) {
            audio[(size_t)l * N_HOP + j] += frames[(size_t)l * N_FFT + j];
            env[(size_t)l * N_HOP + j] += t->hann[j] * t->hann[j];
        }
    std::vector<float> out(audio.begin() + N_PAD, audio.end() - N_PAD);
    for (size_t i = 0; i < out.size(); i++) {
        float e = env[i + N_PAD];
        out[i] = e > 1e-11f ? out[i] / e : 0.0f;
    }
    return out;
}

// ---- vocoder ---------------------------------------------------------------

// Audio for codes[from, to): (to - from) * N_HOP samples, frame f at
// [(f - from) * N_HOP, (f - from + 1) * N_HOP).
int vocode(tts *t, const std::vector<int> &codes, size_t from, size_t to,
           std::vector<float> &audio) {
    const int n = (int)(to - from);
    llama_batch b = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; i++) {
        b.token[i] = codes[from + (size_t)i];
        b.pos[i] = i;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = 0;
        b.logits[i] = 1;
    }
    b.n_tokens = n;
    int enc = llama_encode(t->ccts, b);
    llama_batch_free(b);
    if (enc) return -1;
    std::vector<float> embd((size_t)n * t->n_embd);
    for (int i = 0; i < n; i++) {
        const float *e = llama_get_embeddings_ith(t->ccts, i);
        if (!e) return -1;
        std::copy(e, e + t->n_embd, embd.begin() + (size_t)i * t->n_embd);
    }
    audio = embd_to_audio(t, embd, n);
    return 0;
}

// Streams audio while codes are generated. WavTokenizer is not causal, so
// each window carries LEFT codes of context before the chunk and LOOKAHEAD
// codes after it; only the chunk's interior is emitted. The first XFADE
// samples of a chunk are blended with the previous window's estimate of the
// same samples, hiding the seam.
struct streamer {
    tts *t;
    spore_rt_emit_audio emit;
    void *ectx;
    std::vector<int> codes;
    size_t done = 0;         // codes whose audio has been emitted
    std::vector<float> tail; // previous window's samples just past `done`

    // Returns 0, 1 if cancelled, or -1 on failure.
    int flush(bool final) {
        const size_t left = t->left, ahead = t->ahead, xfade = N_HOP;
        for (;;) {
            size_t n = codes.size();
            if (done >= n) return 0;
            if (t->chunk == 0 && !final) return 0; // whole sentence at once
            size_t chunk = done ? t->chunk : t->first_chunk;
            if (!final && n < done + chunk + ahead) return 0;
            size_t end = final ? n : done + chunk;
            size_t from = done > left ? done - left : 0;
            size_t to = final ? n : std::min(n, end + ahead);
            std::vector<float> audio;
            if (vocode(t, codes, from, to, audio)) return -1;
            size_t a = (done - from) * N_HOP, b = (end - from) * N_HOP;
            for (size_t i = 0; i < tail.size() && a + i < b; i++) {
                float w = (float)(i + 1) / (float)(tail.size() + 1);
                audio[a + i] = tail[i] * (1 - w) + audio[a + i] * w;
            }
            tail.assign(audio.begin() + (long)b,
                        audio.begin() + (long)std::min(audio.size(), b + xfade));
            // At most 200 ms per emit, so the client can start playing early.
            for (size_t i = a; i < b; i += SPORE_OUTETTS_RATE / 5)
                if (emit(ectx, audio.data() + i, std::min<size_t>(SPORE_OUTETTS_RATE / 5, b - i)))
                    return 1;
            done = end;
            if (!final) return 0;
        }
    }
};

// ---- synthesis --------------------------------------------------------------

int synthesize(tts *t, const char *text, size_t len, spore_rt_emit_audio emit,
               void *ectx) {
    std::string words = normalise(text, len, t->sep);
    if (words.empty()) return 0;

    // Guide tokens: after each newline, force the first token of the next
    // word, so the model neither skips nor invents words.
    std::vector<llama_token> guide{t->newline};
    for (size_t p = 0; p <= words.size();) {
        size_t q = words.find(t->sep, p);
        if (q == std::string::npos) q = words.size();
        if (q > p) {
            auto w = tokenize(t->vocab, words.substr(p, q - p), false);
            if (!w.empty()) guide.push_back(w[0]);
        }
        p = q + t->sep.size();
    }

    std::vector<llama_token> prompt = tokenize(t->vocab, "<|im_start|>\n", true);
    for (const std::string &s : {t->audio_text, words, std::string("<|text_end|>\n"),
                                 t->audio_data}) {
        auto v = tokenize(t->vocab, s, false);
        prompt.insert(prompt.end(), v.begin(), v.end());
    }
    const int n_ctx = (int)llama_n_ctx(t->cttc);
    if ((int)prompt.size() >= n_ctx) return -1;

    // In 128-token batches, polling between them: a cancel need not wait
    // for the whole prompt (~0.6 s for 330 tokens on CPU).
    llama_memory_clear(llama_get_memory(t->cttc), true);
    for (size_t i = 0; i < prompt.size(); i += 128) {
        if (i && emit(ectx, nullptr, 0)) return 0; // cancelled
        int32_t n = (int32_t)std::min<size_t>(128, prompt.size() - i);
        if (llama_decode(t->cttc, llama_batch_get_one(prompt.data() + i, n))) return -1;
    }

    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(4));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

    streamer st{t, emit, ectx, {}, 0, {}};
    size_t gi = 0;
    bool use_guide = true;
    int rc = 0;
    for (int n_past = (int)prompt.size(); n_past < n_ctx - 1; n_past++) {
        llama_token tok = llama_sampler_sample(smpl, t->cttc, -1);
        if (gi < guide.size() && use_guide && !llama_vocab_is_control(t->vocab, tok) &&
            !llama_vocab_is_eog(t->vocab, tok))
            tok = guide[gi++];
        use_guide = tok == t->newline;
        if (llama_vocab_is_eog(t->vocab, tok)) break;
        if (tok >= t->code_lo && tok <= t->code_hi) {
            st.codes.push_back(tok - t->code_lo);
            if ((rc = st.flush(false))) break;
        }
        if ((st.codes.size() & 15) == 0 && emit(ectx, nullptr, 0)) { // cancelled?
            rc = 1;
            break;
        }
        if (llama_decode(t->cttc, llama_batch_get_one(&tok, 1))) {
            rc = -1;
            break;
        }
    }
    llama_sampler_free(smpl);
    if (!rc) rc = st.flush(true);
    return rc < 0 ? -1 : 0; /* 1: cancelled, not a failure */
}

int init(tts *t, const spore_outetts_config *cfg) {
    llama_log_set(log_warnings, nullptr);
    llama_backend_init();
    // Generation is memory-bound and prompt evaluation compute-bound; one
    // thread per physical core served both best in measurements.
    unsigned hw = std::thread::hardware_concurrency();
    t->n_threads = cfg->n_threads > 0 ? cfg->n_threads
                                      : (int)std::clamp(hw / 2, 1u, 16u);

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = cfg->n_gpu_layers;
    if (!(t->ttc = llama_model_load_from_file(cfg->model_path, mp))) return -1;
    if (!(t->cts = llama_model_load_from_file(cfg->vocoder_path, mp))) return -1;
    t->vocab = llama_model_get_vocab(t->ttc);

    // A sentence's prompt (~1300 tokens with the speaker profile) plus its
    // codes fits in 4096; the TTS model's KV cache is ~128 KiB per token.
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = cp.n_batch = cp.n_ubatch = 4096;
    cp.n_threads = cp.n_threads_batch = t->n_threads;
    if (!(t->cttc = llama_init_from_model(t->ttc, cp))) return -1;
    cp.embeddings = true;
    if (!(t->ccts = llama_init_from_model(t->cts, cp))) return -1;
    t->n_embd = llama_model_n_embd_out(t->cts);
    if (t->n_embd != N_FFT + 2) {
        fprintf(stderr, "spore_outetts: vocoder width %d, expected %d\n", t->n_embd, N_FFT + 2);
        return -1;
    }

    const char *tmpl = llama_model_chat_template(t->ttc, nullptr);
    t->v03 = tmpl && strstr(tmpl, "outetts-0.3");
    t->sep = t->v03 ? "<|space|>" : "<|text_sep|>";
    t->newline = single_token(t->vocab, "\n");
    t->code_lo = single_token(t->vocab, "<|0|>");
    t->code_hi = single_token(t->vocab, "<|" + std::to_string(N_CODES - 1) + "|>");
    if (t->newline < 0 || t->code_lo < 0 || t->code_hi - t->code_lo != N_CODES - 1) {
        fprintf(stderr, "spore_outetts: %s is not an OuteTTS 0.2/0.3 model\n", cfg->model_path);
        return -1;
    }

    // Streaming windows, in codes (75 per second), measured against
    // whole-sentence vocoding of the same codes: the error is spread, not at
    // seams, and levels off near 32 codes of context and 16 of lookahead.
    // Each lookahead code costs ~7 ms of latency on an RTX 4060.
    t->chunk = cfg->chunk_codes < 0 ? 0 : cfg->chunk_codes ? (size_t)cfg->chunk_codes : 40;
    t->first_chunk = std::min<size_t>(t->chunk, 16);
    t->left = 64;
    t->ahead = 16;

    // The speaker profile: its first `keep` words and their codes.
    int keep = cfg->speaker_words > 0 ? cfg->speaker_words : 1 << 30;
    t->audio_text = "<|text_start|>";
    int k = 0;
    for (const char *p = SPEAKER_WORDS; *p && k < keep; k++) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        t->audio_text.append(p, n);
        t->audio_text += t->sep;
        p += n + (e != nullptr);
    }
    t->audio_data = SPEAKER_CODES; // "<|audio_start|>\n" then one line per word
    size_t end = t->audio_data.find('\n');
    for (int i = 0; i < k && end != std::string::npos; i++)
        end = t->audio_data.find('\n', end + 1);
    if (end != std::string::npos) t->audio_data.resize(end + 1);
    if (t->v03) {
        std::string d;
        for (size_t i = 0; i < t->audio_data.size();) {
            if (!t->audio_data.compare(i, 14, "<|code_start|>")) i += 14;
            else if (!t->audio_data.compare(i, 12, "<|code_end|>")) d += "<|space|>", i += 12;
            else d += t->audio_data[i++];
        }
        t->audio_data = d;
    }

    t->cos_t.resize(N_FFT);
    t->sin_t.resize(N_FFT);
    t->hann.resize(N_FFT);
    for (int i = 0; i < N_FFT; i++) {
        t->cos_t[i] = (float)std::cos(2 * PI * i / N_FFT);
        t->sin_t[i] = (float)std::sin(2 * PI * i / N_FFT);
        t->hann[i] = (float)(0.5 * (1 - std::cos(2 * PI * i / N_FFT))); // periodic
    }
    return 0;
}

} // namespace

extern "C" spore_outetts *spore_outetts_new(const spore_outetts_config *cfg) {
    tts *t = nullptr;
    try {
        t = new tts();
        if (init(t, cfg) == 0) return reinterpret_cast<spore_outetts *>(t);
    } catch (...) {
    }
    spore_outetts_free(reinterpret_cast<spore_outetts *>(t));
    return nullptr;
}

extern "C" void spore_outetts_free(spore_outetts *h) {
    tts *t = reinterpret_cast<tts *>(h);
    if (!t) return;
    if (t->ccts) llama_free(t->ccts);
    if (t->cttc) llama_free(t->cttc);
    if (t->cts) llama_model_free(t->cts);
    if (t->ttc) llama_model_free(t->ttc);
    delete t;
}

extern "C" int spore_outetts_synthesize(void *self, const char *text, size_t len,
                                        const char *voice, spore_rt_emit_audio emit,
                                        void *ctx) {
    (void)voice;
    tts *t = static_cast<tts *>(self);
    std::lock_guard<std::mutex> lock(t->mu);
    try {
        return synthesize(t, text, len, emit, ctx);
    } catch (...) {
        return -1;
    }
}
