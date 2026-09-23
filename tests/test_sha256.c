/* Known-answer tests for the header-only SHA-256 / HMAC-SHA256 used by the
 * WASM session fallback (session.c) and by seq_auth.c. Vectors: FIPS 180-2
 * appendix B (SHA-256) and RFC 4231 section 4 (HMAC-SHA256). */
#include <cwist/core/crypto/sha256.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void to_hex(const uint8_t *d, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 0xf];
    }
    out[2 * n] = '\0';
}

static void expect_digest(const uint8_t *d, const char *expected, const char *label) {
    char hex[2 * CWIST_SHA256_DIGEST_LEN + 1];
    to_hex(d, CWIST_SHA256_DIGEST_LEN, hex);
    if (strcmp(hex, expected) != 0) {
        fprintf(stderr, "%s\n  got      %s\n  expected %s\n", label, hex, expected);
        assert(0);
    }
}

static void expect_sha256(const char *msg, const char *expected) {
    uint8_t d[CWIST_SHA256_DIGEST_LEN];
    cwist_sha256_ctx c;
    cwist_sha256_init(&c);
    cwist_sha256_update(&c, (const uint8_t *)msg, strlen(msg));
    cwist_sha256_final(&c, d);
    expect_digest(d, expected, msg);
}

static void test_fips_vectors(void) {
    expect_sha256("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_sha256("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56 bytes: the length no longer fits the first padding block. */
    expect_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect_sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmno"
                  "pqklmnopqrlmnopqrsmnopqrstnopqrstu",
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    printf("Passed test_fips_vectors\n");
}

/* One million 'a', fed in chunk sizes that straddle the 64-byte block. */
static void test_million_a_in_chunks(void) {
    static uint8_t block[1000];
    memset(block, 'a', sizeof(block));
    const size_t chunks[] = {1, 55, 63, 64, 65, 1000};
    for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        cwist_sha256_ctx c;
        cwist_sha256_init(&c);
        size_t left = 1000000;
        while (left > 0) {
            size_t n = left < chunks[k] ? left : chunks[k];
            cwist_sha256_update(&c, block, n);
            left -= n;
        }
        uint8_t d[CWIST_SHA256_DIGEST_LEN];
        cwist_sha256_final(&c, d);
        expect_digest(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                      "million a");
    }
    printf("Passed test_million_a_in_chunks\n");
}

static void test_hmac_rfc4231(void) {
    uint8_t out[CWIST_SHA256_DIGEST_LEN];

    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    const char *msg1 = "Hi There";
    assert(cwist_hmac_sha256(key1, sizeof(key1), (const uint8_t *)msg1, strlen(msg1), out));
    expect_digest(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                  "RFC 4231 test case 1");

    const char *key2 = "Jefe";
    const char *msg2 = "what do ya want for nothing?";
    assert(cwist_hmac_sha256((const uint8_t *)key2, strlen(key2), (const uint8_t *)msg2,
                             strlen(msg2), out));
    expect_digest(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                  "RFC 4231 test case 2");

    /* A key longer than the block size is hashed first. */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    assert(cwist_hmac_sha256(key6, sizeof(key6), (const uint8_t *)msg6, strlen(msg6), out));
    expect_digest(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
                  "RFC 4231 test case 6");
    printf("Passed test_hmac_rfc4231\n");
}

int main(void) {
    test_fips_vectors();
    test_million_a_in_chunks();
    test_hmac_rfc4231();
    printf("All SHA-256 tests passed!\n");
    return 0;
}
