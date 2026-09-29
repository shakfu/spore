/* SHA-1 (FIPS 180-4 section 6.1), for the WebSocket handshake only.
 * SHA-1 is broken for collision resistance; RFC 6455 uses it as a
 * fixed transform, not for security.
 * SPDX-License-Identifier: MIT */
#include "internal.h"

#include <stdint.h>
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 |
               (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (int t = 16; t < 80; t++)
        w[t] = ROL(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (t < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (t < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t tmp = ROL(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void spore__sha1(const void *data, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    const unsigned char *p = data;
    size_t n = len;
    for (; n >= 64; p += 64, n -= 64) block(h, p);
    /* Padding: 0x80, zeros, then the bit length as 64-bit big-endian. */
    unsigned char tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}
