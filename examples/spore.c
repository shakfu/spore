/* spore: one binary, several subcommands. Run as `llama-server` (a link),
 * it is `spore serve`.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "spore.h"
#if defined(SPORE_WITH_WHISPER) || defined(SPORE_WITH_OUTETTS)
#include "internal.h"
#endif
#ifdef SPORE_WITH_WHISPER
#include "spore_whisper.h"
#endif
#ifdef SPORE_WITH_OUTETTS
#include "spore_outetts.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cli_is(const char *a, const char *const *names) {
    for (; *names; names++)
        if (!strcmp(a, *names)) return 1;
    return 0;
}

int cli_missing(const char *flag) {
    fprintf(stderr, "spore: %s needs a value\n", flag);
    exit(2);
}

long cli_long(const char *flag, const char *v, long lo, long hi) {
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || end == v || *end || n < lo || n > hi) {
        fprintf(stderr, "spore: %s must be an integer in %ld..%ld, not '%s'\n", flag, lo,
                hi, v);
        exit(2);
    }
    return n;
}

void cli_unknown(const char *cmd, const char *arg) {
    fprintf(stderr, "spore %s: unknown option %s (see 'spore %s --help')\n", cmd, arg, cmd);
    exit(2);
}

void cli_too_many(const char *flag, int max) {
    fprintf(stderr, "spore: at most %d %s flags\n", max, flag);
    exit(2);
}

static const char *const modules = "core"
#ifdef SPORE_WITH_WS
    " ws"
#endif
#ifdef SPORE_WITH_LLM
    " llm"
#endif
#ifdef SPORE_WITH_REALTIME
    " realtime"
#endif
#ifdef SPORE_WITH_LLAMA
    " llama"
#endif
#ifdef SPORE_WITH_WHISPER
    " whisper"
#endif
#ifdef SPORE_WITH_OUTETTS
    " outetts"
#endif
    "";

static void usage(FILE *f) {
    fprintf(f, "usage: spore <command> [options]\n"
               "  serve       HTTP server with the OpenAI-compatible API, in place of llama-server\n"
               "  transcribe  print the speech in a WAV file as text (whisper.cpp)\n"
               "  speak       synthesize text into a WAV file (OuteTTS)\n"
               "  token       write a new random API key to a file with mode 0600\n"
               "  modules     list the compiled-in modules and engines\n"
               "  version     print the version\n"
               "'spore <command> --help' lists a command's options.\n"
               "A link to spore named llama-server runs 'serve'.\n");
}

static int help_only(int argc, char **argv, const char *text) {
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fputs(text, stdout);
        return 0;
    }
    fputs(text, stderr);
    return 2;
}

static int cmd_version(int argc, char **argv) {
    if (argc > 1) return help_only(argc, argv, "usage: spore version\n");
    printf("spore %s\n", SPORE_VERSION);
    return 0;
}

static int cmd_modules(int argc, char **argv) {
    if (argc > 1) return help_only(argc, argv, "usage: spore modules\n");
    printf("%s\n", modules);
    return 0;
}

/* 32 random bytes as hex, written to `path` with mode 0600. The file is
 * created beside `path` and renamed over it, so an existing file's mode or
 * open descriptors never see the key. */
static int write_token_file(const char *path) {
    unsigned char r[32];
    char out[65];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, r, sizeof r);
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof r) return -1;
    for (size_t i = 0; i < sizeof r; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) return -1;
    if ((fd = mkstemp(tmp)) < 0) return -1; /* mode 0600 */
    int ok = write(fd, out, 64) == 64 && write(fd, "\n", 1) == 1;
    ok = close(fd) == 0 && ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

static int cmd_token(int argc, char **argv) {
    static const char text[] =
        "usage: spore token FILE\n"
        "Write a new random key to FILE (mode 0600), for 'spore serve --api-key-file FILE'.\n";
    if (argc != 2 || argv[1][0] == '-') return help_only(argc, argv, text);
    if (write_token_file(argv[1])) {
        fprintf(stderr, "spore: cannot write %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

#if defined(SPORE_WITH_WHISPER) || defined(SPORE_WITH_OUTETTS)
static int positional(const char *a) {
    return a[0] != '-' || !a[1];
}
#endif

#ifdef SPORE_WITH_WHISPER
static uint32_t le(const unsigned char *p, int bytes) {
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

/* A 16-bit PCM WAV, mixed down to mono. Returns malloc'd samples, or NULL
 * with *err set. */
static float *read_wav(const char *path, size_t *n, int *rate, const char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = strerror(errno);
        return NULL;
    }
    unsigned char *b = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            unsigned char *nb = realloc(b, cap ? cap * 2 : 1 << 16);
            if (!nb) break;
            b = nb;
            cap = cap ? cap * 2 : 1 << 16;
        }
        size_t got = fread(b + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    int bad = ferror(f) || !feof(f); /* a read error, or out of memory */
    fclose(f);
    *err = bad ? "cannot read the whole file" : "not a RIFF WAVE file";
    float *out = NULL;
    if (bad || len < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) goto done;
    int channels = 0, bits = 0;
    for (size_t p = 12; p + 8 <= len;) {
        size_t size = le(b + p + 4, 4), body = p + 8;
        if (size > len - body) size = len - body; /* truncated or streamed */
        if (!memcmp(b + p, "fmt ", 4) && size >= 16) {
            channels = (int)le(b + body + 2, 2);
            *rate = (int)le(b + body + 4, 4);
            bits = (int)le(b + body + 14, 2);
            if (le(b + body, 2) != 1 || bits != 16 || channels < 1 || *rate <= 0) {
                *err = "only 16-bit PCM WAV is supported";
                goto done;
            }
        } else if (!memcmp(b + p, "data", 4) && channels) {
            *n = size / 2 / (size_t)channels;
            if (!(out = malloc((*n ? *n : 1) * sizeof *out))) {
                *err = "out of memory";
                goto done;
            }
            const unsigned char *s = b + body;
            for (size_t i = 0; i < *n; i++) {
                float sum = 0;
                for (int c = 0; c < channels; c++, s += 2)
                    sum += (float)(int16_t)le(s, 2) / 32768.0f;
                out[i] = sum / (float)channels;
            }
            goto done;
        }
        p = body + size + (size & 1);
    }
done:
    free(b);
    return out;
}

static int cmd_transcribe(int argc, char **argv) {
    static const char text[] =
        "usage: spore transcribe -m MODEL [options] FILE.wav\n"
        "  -m, --model PATH     whisper.cpp model, e.g. ggml-base.en.bin\n"
        "  -l, --language L     spoken language, or auto (default en)\n"
        "  -t, --threads N      threads (default 4)\n"
        "  --gpu                run on the GPU (needs a GPU ggml build)\n"
        "FILE is 16-bit PCM at any rate; channels are mixed down.\n";
    spore_whisper_config wcfg = {0};
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (CLI_FLAG("-h", "--help")) {
            fputs(text, stdout);
            return 0;
        }
        else if (CLI_ARG("-m", "--model")) wcfg.model_path = v;
        else if (CLI_ARG("-l", "--language")) wcfg.language = v;
        else if (CLI_ARG("-t", "--threads")) wcfg.n_threads = (int)cli_long(a, v, 1, 1024);
        else if (CLI_FLAG("--gpu")) wcfg.use_gpu = 1;
        else if (positional(a) && !file) file = a;
        else cli_unknown("transcribe", a);
    }
    if (!wcfg.model_path || !file) {
        fputs(text, stderr);
        return 2;
    }
    const char *err;
    size_t n = 0, m = 0;
    int rate = 0;
    float *pcm = read_wav(file, &n, &rate, &err);
    if (!pcm) {
        fprintf(stderr, "spore: %s: %s\n", file, err);
        return 1;
    }
    float *in = spore__resample(pcm, n, rate, SPORE_WHISPER_RATE, &m);
    free(pcm);
    spore_whisper *w = in ? spore_whisper_new(&wcfg) : NULL;
    if (!w) {
        fprintf(stderr, "spore: cannot load %s\n", in ? wcfg.model_path : "audio");
        free(in);
        return 1;
    }
    spore_buf out = {0};
    int rc = spore_whisper_transcribe(w, in, m, &out) || out.err;
    if (!rc) {
        const char *t = out.ptr ? out.ptr : "";
        size_t k = out.len;
        while (k && *t == ' ') t++, k--;
        printf("%.*s\n", (int)k, t);
    } else {
        fprintf(stderr, "spore: transcription failed\n");
    }
    spore_buf_free(&out);
    spore_whisper_free(w);
    free(in);
    return rc;
}
#endif

#ifdef SPORE_WITH_OUTETTS
typedef struct {
    float *pcm;
    size_t n, cap;
} pcm_buf;

static int collect(void *ctx, const float *pcm, size_t n) {
    pcm_buf *b = ctx;
    if (b->n + n > b->cap) {
        size_t cap = (b->n + n) * 2;
        float *p = realloc(b->pcm, cap * sizeof *p);
        if (!p) return 1;
        b->pcm = p;
        b->cap = cap;
    }
    memcpy(b->pcm + b->n, pcm, n * sizeof *pcm);
    b->n += n;
    return 0;
}

static int cmd_speak(int argc, char **argv) {
    static const char text[] =
        "usage: spore speak -m MODEL --vocoder PATH [options] TEXT...\n"
        "  -m, --model PATH     OuteTTS 0.2/0.3 GGUF\n"
        "  --vocoder PATH       WavTokenizer GGUF\n"
        "  -o, --output FILE    write the WAV here (default: stdout, unless a terminal)\n"
        "  -t, --threads N      threads (default: physical cores, at most 16)\n"
        "  --tts-speaker-words N  shorter speaker profile, faster (default 30)\n"
        "  --gpu                offload to the GPU (needs a GPU ggml build)\n"
        "Writes 16-bit mono PCM at 24 kHz.\n";
    spore_outetts_config tcfg = {.chunk_codes = -1}; /* no streaming to a file */
    const char *path = NULL;
    spore_buf say = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (CLI_FLAG("-h", "--help")) {
            fputs(text, stdout);
            spore_buf_free(&say);
            return 0;
        }
        else if (CLI_ARG("-m", "--model")) tcfg.model_path = v;
        else if (CLI_ARG("--vocoder")) tcfg.vocoder_path = v;
        else if (CLI_ARG("-o", "--output")) path = v;
        else if (CLI_ARG("-t", "--threads")) tcfg.n_threads = (int)cli_long(a, v, 1, 1024);
        else if (CLI_ARG("--tts-speaker-words"))
            tcfg.speaker_words = (int)cli_long(a, v, 1, 30);
        else if (CLI_FLAG("--gpu")) tcfg.n_gpu_layers = 999;
        else if (positional(a)) {
            if (say.len) spore_buf_puts(&say, " ");
            spore_buf_puts(&say, a);
        }
        else cli_unknown("speak", a);
    }
    if (!tcfg.model_path || !tcfg.vocoder_path || !say.len || say.err) {
        fputs(text, stderr);
        spore_buf_free(&say);
        return 2;
    }
    if (!path && isatty(STDOUT_FILENO)) {
        fprintf(stderr, "spore: not writing audio to a terminal; use -o FILE\n");
        spore_buf_free(&say);
        return 2;
    }
    spore_outetts *t = spore_outetts_new(&tcfg);
    if (!t) {
        fprintf(stderr, "spore: cannot load %s with vocoder %s\n", tcfg.model_path,
                tcfg.vocoder_path);
        spore_buf_free(&say);
        return 1;
    }
    pcm_buf b = {0};
    int rc = spore_outetts_synthesize(t, say.ptr, say.len, NULL, collect, &b);
    spore_outetts_free(t);
    spore_buf_free(&say);
    unsigned char *w = !rc && b.n <= (0xFFFFFFFFu - 44) / 2 ? malloc(44 + b.n * 2) : NULL;
    if (!w) {
        fprintf(stderr, "spore: synthesis failed\n");
        free(b.pcm);
        return 1;
    }
    spore__wav_header(w, (uint32_t)(b.n * 2), SPORE_OUTETTS_RATE);
    spore__float_to_pcm16le(b.pcm, b.n, w + 44);
    free(b.pcm);
    FILE *f = path ? fopen(path, "wb") : stdout;
    int ok = f && fwrite(w, 1, 44 + b.n * 2, f) == 44 + b.n * 2;
    ok = f && (path ? fclose(f) : fflush(f)) == 0 && ok;
    free(w);
    if (!ok) {
        fprintf(stderr, "spore: cannot write %s: %s\n", path ? path : "stdout", strerror(errno));
        if (path) unlink(path);
        return 1;
    }
    return 0;
}
#endif

#if !defined(SPORE_WITH_WHISPER) || !defined(SPORE_WITH_OUTETTS)
/* Subcommands whose engine was not built say how to get it. */
static int not_built(const char *cmd, const char *engine) {
    fprintf(stderr, "spore: '%s' needs %s; this spore was built without it (make engines)\n",
            cmd, engine);
    return 2;
}
#endif

int main(int argc, char **argv) {
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "llama-server")) return serve_main(argc, argv);
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "--version")) return cmd_version(1, argv + 1);
    if (!strcmp(cmd, "serve")) return serve_main(argc - 1, argv + 1);
    if (!strcmp(cmd, "version")) return cmd_version(argc - 1, argv + 1);
    if (!strcmp(cmd, "modules")) return cmd_modules(argc - 1, argv + 1);
    if (!strcmp(cmd, "token")) return cmd_token(argc - 1, argv + 1);
#ifdef SPORE_WITH_WHISPER
    if (!strcmp(cmd, "transcribe")) return cmd_transcribe(argc - 1, argv + 1);
#else
    if (!strcmp(cmd, "transcribe")) return not_built(cmd, "whisper.cpp");
#endif
#ifdef SPORE_WITH_OUTETTS
    if (!strcmp(cmd, "speak")) return cmd_speak(argc - 1, argv + 1);
#else
    if (!strcmp(cmd, "speak")) return not_built(cmd, "OuteTTS");
#endif
    if (*cmd) fprintf(stderr, "spore: unknown command '%s'\n", cmd);
    usage(stderr);
    return 2;
}
