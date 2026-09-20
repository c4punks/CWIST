/**
 * @file sha256.h
 * @brief Minimal self-contained SHA-256 and HMAC-SHA256 (RFC 4634 style).
 *
 * Header-only (all functions static): exists so the WASM build can verify
 * session-cookie signatures without pulling OpenSSL/boringssl into
 * WASM_SRCS.  The native build keeps using OpenSSL HMAC; this header is the
 * Emscripten (and any other constrained-target) fallback selected in
 * session.c.  Not a general-purpose crypto API - just what signed sessions
 * need.
 */

#ifndef __CWIST_CORE_CRYPTO_SHA256_H__
#define __CWIST_CORE_CRYPTO_SHA256_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define CWIST_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} cwist_sha256_ctx;

static inline uint32_t cwist_sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static inline void cwist_sha256_transform(cwist_sha256_ctx *ctx, const uint8_t *data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t m[64];
    for (size_t i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = cwist_sha256_rotr(m[i - 15], 7) ^ cwist_sha256_rotr(m[i - 15], 18) ^
                      (m[i - 15] >> 3);
        uint32_t s1 = cwist_sha256_rotr(m[i - 2], 17) ^ cwist_sha256_rotr(m[i - 2], 19) ^
                      (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = cwist_sha256_rotr(e, 6) ^ cwist_sha256_rotr(e, 11) ^ cwist_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + m[i];
        uint32_t s0 = cwist_sha256_rotr(a, 2) ^ cwist_sha256_rotr(a, 13) ^ cwist_sha256_rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static inline void cwist_sha256_init(cwist_sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static inline void cwist_sha256_update(cwist_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            cwist_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static inline void cwist_sha256_final(cwist_sha256_ctx *ctx, uint8_t out[CWIST_SHA256_DIGEST_LEN]) {
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        cwist_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (int j = 7; j >= 0; j--) ctx->data[63 - (7 - j)] = (uint8_t)(ctx->bitlen >> (j * 8));
    cwist_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/** One-shot HMAC-SHA256 (RFC 2104). Returns true on success. */
static inline bool cwist_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                                     size_t msg_len, uint8_t out[CWIST_SHA256_DIGEST_LEN]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        cwist_sha256_ctx c;
        cwist_sha256_init(&c);
        cwist_sha256_update(&c, key, key_len);
        cwist_sha256_final(&c, k);
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    cwist_sha256_ctx c;
    uint8_t inner[CWIST_SHA256_DIGEST_LEN];
    cwist_sha256_init(&c);
    cwist_sha256_update(&c, ipad, sizeof(ipad));
    cwist_sha256_update(&c, msg, msg_len);
    cwist_sha256_final(&c, inner);

    cwist_sha256_init(&c);
    cwist_sha256_update(&c, opad, sizeof(opad));
    cwist_sha256_update(&c, inner, sizeof(inner));
    cwist_sha256_final(&c, out);
    return true;
}

#endif /* __CWIST_CORE_CRYPTO_SHA256_H__ */
