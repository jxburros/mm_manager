/*
 * SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 (FIPS 180-4, RFC 2104, RFC 8018).
 * Used by the control panel (mm_gui) to store account passwords.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_SHA256_H_
#define MM_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#define MM_SHA256_LEN   32

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   used;
} mm_sha256_t;

void mm_sha256_init(mm_sha256_t *c);
void mm_sha256_update(mm_sha256_t *c, const void *data, size_t len);
void mm_sha256_final(mm_sha256_t *c, uint8_t out[MM_SHA256_LEN]);
void mm_sha256(const void *data, size_t len, uint8_t out[MM_SHA256_LEN]);

void mm_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[MM_SHA256_LEN]);

/* Derive out_len bytes from password and salt. */
void mm_pbkdf2_sha256(const void *password, size_t pass_len, const void *salt, size_t salt_len,
                      uint32_t iterations, uint8_t *out, size_t out_len);

#endif /* MM_SHA256_H_ */
