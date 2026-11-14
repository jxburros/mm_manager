/*
 * SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 (FIPS 180-4, RFC 2104, RFC 8018).
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <string.h>

#include "mm_sha256.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n)   (((x) >> (n)) | ((x) << (32 - (n))))

static void block(mm_sha256_t *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void mm_sha256_init(mm_sha256_t *c) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0;
    c->used = 0;
}

void mm_sha256_update(mm_sha256_t *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    c->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - c->used;

        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p       += n;
        len     -= n;
        if (c->used == 64) {
            block(c, c->buf);
            c->used = 0;
        }
    }
}

void mm_sha256_final(mm_sha256_t *c, uint8_t out[MM_SHA256_LEN]) {
    uint64_t bits = c->bits;
    uint8_t  pad  = 0x80;
    uint8_t  len[8];

    mm_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56) {
        mm_sha256_update(c, &pad, 1);
    }
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    mm_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void mm_sha256(const void *data, size_t len, uint8_t out[MM_SHA256_LEN]) {
    mm_sha256_t c;

    mm_sha256_init(&c);
    mm_sha256_update(&c, data, len);
    mm_sha256_final(&c, out);
}

/* The inner and outer hashes after the padded key, so PBKDF2 only pays for them once. */
typedef struct {
    mm_sha256_t inner, outer;
} hmac_t;

static void hmac_init(hmac_t *m, const void *key, size_t key_len) {
    uint8_t k[64] = { 0 }, pad[64];

    if (key_len > 64) {
        mm_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    mm_sha256_init(&m->inner);
    mm_sha256_update(&m->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    mm_sha256_init(&m->outer);
    mm_sha256_update(&m->outer, pad, 64);
}

static void hmac_run(const hmac_t *m, const void *data, size_t len, uint8_t out[MM_SHA256_LEN]) {
    mm_sha256_t c = m->inner;
    uint8_t     ih[MM_SHA256_LEN];

    mm_sha256_update(&c, data, len);
    mm_sha256_final(&c, ih);
    c = m->outer;
    mm_sha256_update(&c, ih, sizeof(ih));
    mm_sha256_final(&c, out);
}

void mm_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[MM_SHA256_LEN]) {
    hmac_t m;

    hmac_init(&m, key, key_len);
    hmac_run(&m, data, len, out);
}

void mm_pbkdf2_sha256(const void *password, size_t pass_len, const void *salt, size_t salt_len,
                      uint32_t iterations, uint8_t *out, size_t out_len) {
    hmac_t   m;
    uint8_t  msg[128 + 4];
    uint32_t blk = 1;

    if (salt_len > 128) {
        salt_len = 128;
    }
    hmac_init(&m, password, pass_len);
    memcpy(msg, salt, salt_len);
    while (out_len > 0) {
        uint8_t u[MM_SHA256_LEN], t[MM_SHA256_LEN];
        size_t  n = out_len < MM_SHA256_LEN ? out_len : MM_SHA256_LEN;

        msg[salt_len]     = (uint8_t)(blk >> 24);
        msg[salt_len + 1] = (uint8_t)(blk >> 16);
        msg[salt_len + 2] = (uint8_t)(blk >> 8);
        msg[salt_len + 3] = (uint8_t)blk;
        hmac_run(&m, msg, salt_len + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&m, u, sizeof(u), u);
            for (int k = 0; k < MM_SHA256_LEN; k++) t[k] ^= u[k];
        }
        memcpy(out, t, n);
        out     += n;
        out_len -= n;
        blk++;
    }
}
