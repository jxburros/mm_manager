/*
 * Checks SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 against published test vectors.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <string.h>

#include "src/mm_sha256.h"

static int failures;

static void expect(const char *what, const uint8_t *got, size_t n, const char *want_hex) {
    char hex[2 * 64 + 1];

    for (size_t i = 0; i < n; i++) {
        sprintf(hex + 2 * i, "%02x", got[i]);
    }
    if (strcmp(hex, want_hex) != 0) {
        printf("FAIL %s\n  got  %s\n  want %s\n", what, hex, want_hex);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void) {
    uint8_t out[64];
    char    million[1000];

    /* FIPS 180-4 examples. */
    mm_sha256("", 0, out);
    expect("sha256 empty", out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    mm_sha256("abc", 3, out);
    expect("sha256 abc", out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    mm_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    expect("sha256 two blocks", out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        mm_sha256_t c;

        memset(million, 'a', sizeof(million));
        mm_sha256_init(&c);
        for (int i = 0; i < 1000; i++) {
            mm_sha256_update(&c, million, sizeof(million));
        }
        mm_sha256_final(&c, out);
        expect("sha256 a million a's", out, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* RFC 4231 test case 2. */
    mm_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    expect("hmac-sha256 rfc4231 #2", out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    /* RFC 4231 test case 6: a key longer than a block. */
    {
        uint8_t key[131];

        memset(key, 0xaa, sizeof(key));
        mm_hmac_sha256(key, sizeof(key), "Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
        expect("hmac-sha256 rfc4231 #6", out, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }

    /* PBKDF2-HMAC-SHA256 (RFC 7914 section 11, and the widely used RFC 6070-style vectors). */
    mm_pbkdf2_sha256("password", 8, "salt", 4, 1, out, 32);
    expect("pbkdf2 c=1", out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    mm_pbkdf2_sha256("password", 8, "salt", 4, 4096, out, 32);
    expect("pbkdf2 c=4096", out, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    mm_pbkdf2_sha256("passwd", 6, "salt", 4, 1, out, 64);
    expect("pbkdf2 rfc7914 64 bytes", out, 64,
           "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
           "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");

    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures != 0;
}
