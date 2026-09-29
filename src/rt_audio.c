/* Audio helpers for the realtime module: base64, PCM, resampling.
 * SPDX-License-Identifier: MIT */
#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* RFC 4648 section 4, padding required. Output never outruns input. */
long spore__base64_decode(char *s, size_t n) {
    if (n % 4) return -1;
    unsigned char *o = (unsigned char *)s;
    for (size_t i = 0; i < n; i += 4) {
        int v[4], pad = 0;
        for (int k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)s[i + k];
            if (c == '=' && i + 4 == n && k >= 2) {
                v[k] = 0;
                pad++;
            } else if (pad || (v[k] = b64val(c)) < 0) {
                return -1;
            }
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 |
                     (uint32_t)v[2] << 6 | (uint32_t)v[3];
        *o++ = (unsigned char)(w >> 16);
        if (pad < 2) *o++ = (unsigned char)(w >> 8);
        if (pad < 1) *o++ = (unsigned char)w;
    }
    return (long)(o - (unsigned char *)s);
}

void spore__pcm16_to_float(const int16_t *in, size_t n, float *out) {
    for (size_t i = 0; i < n; i++) out[i] = (float)in[i] / 32768.0f;
}

void spore__float_to_pcm16le(const float *in, size_t n, unsigned char *out) {
    for (size_t i = 0; i < n; i++) {
        float f = in[i] * 32767.0f;
        long v = f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : lrintf(f);
        out[2 * i] = (unsigned char)(v & 0xFF);
        out[2 * i + 1] = (unsigned char)((v >> 8) & 0xFF);
    }
}

#define PI 3.14159265358979323846

static int gcd(int a, int b) {
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Windowed-sinc resampling of a whole buffer (Hann window, 16 zero
 * crossings per side). The cutoff follows the lower rate, so downsampling
 * is anti-aliased. With to/from = L/M in lowest terms, output sample i sits
 * at input position i*M/L, so only L kernel phases exist; they are built
 * once. Returns a malloc'd buffer, or NULL. */
float *spore__resample(const float *in, size_t n, int from, int to,
                       size_t *out_n) {
    if (from <= 0 || to <= 0) return NULL;
    size_t m = from == to ? n : (size_t)((double)n * to / from);
    float *out = malloc((m ? m : 1) * sizeof *out);
    if (!out) return NULL;
    *out_n = m;
    if (from == to) {
        for (size_t i = 0; i < n; i++) out[i] = in[i];
        return out;
    }
    const int g = gcd(from, to), L = to / g, M = from / g;
    const double fc = to < from ? (double)to / from : 1; /* in input Nyquists */
    const double half = 16 / fc;         /* kernel half-width, input samples */
    const int taps = 2 * (int)ceil(half) + 1;
    const long first = -(long)ceil(half); /* tap offset from floor(t) */
    if ((size_t)L * (size_t)taps > (1u << 22)) { /* absurd rate pair */
        free(out);
        return NULL;
    }
    float *kern = malloc((size_t)L * taps * sizeof *kern);
    if (!kern) {
        free(out);
        return NULL;
    }
    for (int p = 0; p < L; p++) {
        double frac = (double)p / L; /* t - floor(t) */
        double norm = 0;
        float *kp = kern + (size_t)p * taps;
        for (int j = 0; j < taps; j++) {
            double d = frac - (first + j); /* t - k */
            double h = 0;
            if (fabs(d) < half) {
                double x = d * fc;
                double sinc = x == 0 ? 1 : sin(PI * x) / (PI * x);
                h = sinc * (0.5 + 0.5 * cos(PI * d / half));
            }
            kp[j] = (float)h;
            norm += h;
        }
        for (int j = 0; j < taps; j++) kp[j] = (float)(kp[j] / norm); /* DC gain 1 */
    }
    for (size_t i = 0; i < m; i++) {
        uint64_t num = (uint64_t)i * (uint64_t)M;
        long base = (long)(num / (uint64_t)L);
        const float *kp = kern + (size_t)(num % (uint64_t)L) * taps;
        double acc = 0;
        for (int j = 0; j < taps; j++) {
            long k = base + first + j;
            if (k >= 0 && (size_t)k < n) acc += kp[j] * in[k];
        }
        out[i] = (float)acc;
    }
    free(kern);
    return out;
}
