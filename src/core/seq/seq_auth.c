#include <cwist/core/seq/seq_auth.h>
#include <cwist/core/mem/alloc.h>

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
/* WASM links no OpenSSL: HMAC-SHA256 comes from the bundled header-only
 * implementation and entropy from the host (crypto.getRandomValues under
 * Emscripten, getentropy/__wasi_random_get under WASI). */
#include <cwist/core/crypto/sha256.h>
#else
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#endif
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#if defined(__wasi__)
#include <unistd.h> /* getentropy */
#endif

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
#define SEQ_AUTH_WASM_CRYPTO 1
#define SEQ_AUTH_TAG_CAP CWIST_SHA256_DIGEST_LEN
/* Constant-time compare + secure cleanse without OpenSSL. */
static int seq_auth_memcmp(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return (int)diff;
}
#define SEQ_AUTH_MEMCMP(a, b, len) seq_auth_memcmp((a), (b), (len))
#define SEQ_AUTH_CLEANSE(ptr, len) memset((ptr), 0, (len))
#else
#define SEQ_AUTH_TAG_CAP EVP_MAX_MD_SIZE
#define SEQ_AUTH_MEMCMP(a, b, len) CRYPTO_memcmp((a), (b), (len))
#define SEQ_AUTH_CLEANSE(ptr, len) OPENSSL_cleanse((ptr), (len))
#endif

#if defined(__EMSCRIPTEN__)
EM_JS(int, seq_auth_random_js, (uint8_t *buf, int len), {
    // clang-format off - JS body, not C: keep make format away from it
    if (typeof crypto == = 'undefined' || !crypto.getRandomValues) return 0;
    crypto.getRandomValues(new Uint8Array(Module.HEAPU8.buffer, buf, len));
    return 1;
    // clang-format on
});
#endif

struct cwist_seq_auth_context {
    uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE];
    uint8_t session_id[CWIST_SEQ_AUTH_SESSION_ID_SIZE];
    uint8_t *nonces;
    uint8_t *completed_ids;
    size_t capacity;
    size_t nonce_count;
    size_t completed_count;
    size_t nonce_next;
    size_t completed_next;
    pthread_mutex_t lock;
};

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool seq_auth_chunk_valid(const uint8_t *data, size_t len, cwist_seq_chunk_t *out) {
    if (!data || len < CWIST_SEQ_HEADER_SIZE || !out) return false;
    out->seq = read_u16(data);
    out->total = read_u16(data + 2);
    out->payload_len = read_u16(data + 4);
    out->chunk_size = read_u16(data + 6);
    if (!out->seq || !out->total || out->total > CWIST_SEQ_MAX_CHUNKS || out->seq > out->total ||
        !out->payload_len || !out->chunk_size || out->payload_len > out->chunk_size ||
        (out->seq != out->total && out->payload_len != out->chunk_size) ||
        len < (size_t)CWIST_SEQ_AUTH_HEADER_SIZE + out->payload_len)
        return false;
    out->payload = data + CWIST_SEQ_AUTH_HEADER_SIZE;
    return true;
}

static bool auth_tag(const cwist_seq_auth_context_t *ctx, const uint8_t *header_without_tag,
                     size_t header_len, const uint8_t *payload, size_t payload_len,
                     uint8_t tag[SEQ_AUTH_TAG_CAP]) {
#ifdef SEQ_AUTH_WASM_CRYPTO
    /* Streaming HMAC-SHA256 (RFC 2104) over session_id || header || payload.
     * The 32-byte key never exceeds the 64-byte block size, so no key
     * pre-hashing is needed. */
    uint8_t k[64] = {0};
    memcpy(k, ctx->key, sizeof(ctx->key));
    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    cwist_sha256_ctx c;
    uint8_t inner[CWIST_SHA256_DIGEST_LEN];
    cwist_sha256_init(&c);
    cwist_sha256_update(&c, ipad, sizeof(ipad));
    cwist_sha256_update(&c, ctx->session_id, sizeof(ctx->session_id));
    cwist_sha256_update(&c, header_without_tag, header_len);
    cwist_sha256_update(&c, payload, payload_len);
    cwist_sha256_final(&c, inner);
    cwist_sha256_init(&c);
    cwist_sha256_update(&c, opad, sizeof(opad));
    cwist_sha256_update(&c, inner, sizeof(inner));
    cwist_sha256_final(&c, tag);
    SEQ_AUTH_CLEANSE(ipad, sizeof(ipad));
    SEQ_AUTH_CLEANSE(opad, sizeof(opad));
    SEQ_AUTH_CLEANSE(inner, sizeof(inner));
    return true;
#else
    HMAC_CTX *h = HMAC_CTX_new();
    unsigned int tag_len = 0;
    bool ok = h && HMAC_Init_ex(h, ctx->key, sizeof(ctx->key), EVP_sha256(), NULL) == 1 &&
              HMAC_Update(h, ctx->session_id, sizeof(ctx->session_id)) == 1 &&
              HMAC_Update(h, header_without_tag, header_len) == 1 &&
              HMAC_Update(h, payload, payload_len) == 1 && HMAC_Final(h, tag, &tag_len) == 1 &&
              tag_len >= CWIST_SEQ_AUTH_TAG_SIZE;
    HMAC_CTX_free(h);
    return ok;
#endif
}

static bool cache_contains(const uint8_t *cache, size_t count, size_t width, const uint8_t *value) {
    for (size_t i = 0; i < count; ++i)
        if (SEQ_AUTH_MEMCMP(cache + i * width, value, width) == 0) return true;
    return false;
}

static void cache_add(uint8_t *cache, size_t *count, size_t *next, size_t capacity, size_t width,
                      const uint8_t *value) {
    memcpy(cache + *next * width, value, width);
    *next = (*next + 1) % capacity;
    if (*count < capacity) (*count)++;
}

cwist_seq_auth_context_t *
cwist_seq_auth_context_create(const uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE],
                              const uint8_t session_id[CWIST_SEQ_AUTH_SESSION_ID_SIZE],
                              size_t replay_window) {
    if (!key || !session_id || replay_window == 0 || replay_window > SIZE_MAX / 28) return NULL;
    cwist_seq_auth_context_t *ctx = cwist_alloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        cwist_free(ctx);
        return NULL;
    }
    ctx->nonces = cwist_alloc_array(replay_window, CWIST_SEQ_AUTH_NONCE_SIZE);
    ctx->completed_ids = cwist_alloc_array(replay_window, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    if (!ctx->nonces || !ctx->completed_ids) {
        cwist_seq_auth_context_destroy(ctx);
        return NULL;
    }
    memcpy(ctx->key, key, sizeof(ctx->key));
    memcpy(ctx->session_id, session_id, sizeof(ctx->session_id));
    ctx->capacity = replay_window;
    return ctx;
}

void cwist_seq_auth_context_destroy(cwist_seq_auth_context_t *ctx) {
    if (!ctx) return;
    SEQ_AUTH_CLEANSE(ctx->key, sizeof(ctx->key));
    SEQ_AUTH_CLEANSE(ctx->session_id, sizeof(ctx->session_id));
    pthread_mutex_destroy(&ctx->lock);
    cwist_free(ctx->nonces);
    cwist_free(ctx->completed_ids);
    cwist_free(ctx);
}

bool cwist_seq_auth_random(uint8_t *out, size_t len) {
    if (!out || len == 0 || len > INT_MAX) return false;
#if defined(__EMSCRIPTEN__)
    return seq_auth_random_js(out, (int)len) == 1;
#elif defined(__wasi__)
    return getentropy(out, len) == 0;
#else
    return RAND_bytes(out, (int)len) == 1;
#endif
}

bool cwist_seq_auth_wrap(const cwist_seq_auth_context_t *ctx,
                         const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE],
                         const uint8_t nonce[CWIST_SEQ_AUTH_NONCE_SIZE], const uint8_t *chunk,
                         size_t chunk_len, uint8_t **out, size_t *out_len) {
    cwist_seq_chunk_t parsed;
    if (!ctx || !message_id || !nonce || !out || !out_len ||
        !cwist_seq_chunk_parse(chunk, chunk_len, &parsed) ||
        chunk_len != (size_t)CWIST_SEQ_HEADER_SIZE + parsed.payload_len)
        return false;
    uint8_t *wire = cwist_alloc(CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len);
    if (!wire) return false;
    memcpy(wire, chunk, CWIST_SEQ_HEADER_SIZE);
    memcpy(wire + CWIST_SEQ_HEADER_SIZE, message_id, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    memcpy(wire + CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE, nonce,
           CWIST_SEQ_AUTH_NONCE_SIZE);
    memcpy(wire + CWIST_SEQ_AUTH_HEADER_SIZE, parsed.payload, parsed.payload_len);
    uint8_t tag[SEQ_AUTH_TAG_CAP];
    if (!auth_tag(ctx, wire,
                  CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
                      CWIST_SEQ_AUTH_NONCE_SIZE,
                  parsed.payload, parsed.payload_len, tag)) {
        cwist_free(wire);
        return false;
    }
    memcpy(wire + CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
               CWIST_SEQ_AUTH_NONCE_SIZE,
           tag, CWIST_SEQ_AUTH_TAG_SIZE);
    *out = wire;
    *out_len = CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len;
    return true;
}

bool cwist_seq_auth_unwrap(cwist_seq_auth_context_t *ctx, const uint8_t *data, size_t len,
                           cwist_seq_auth_metadata_t *metadata, cwist_seq_chunk_t *chunk) {
    cwist_seq_chunk_t parsed;
    if (!ctx || !metadata || !chunk || !seq_auth_chunk_valid(data, len, &parsed) ||
        len != (size_t)CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len)
        return false;
    const uint8_t *message_id = data + CWIST_SEQ_HEADER_SIZE;
    const uint8_t *nonce = message_id + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE;
    const uint8_t *tag = nonce + CWIST_SEQ_AUTH_NONCE_SIZE;
    uint8_t expected[SEQ_AUTH_TAG_CAP];
    if (!auth_tag(ctx, data,
                  CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
                      CWIST_SEQ_AUTH_NONCE_SIZE,
                  parsed.payload, parsed.payload_len, expected) ||
        SEQ_AUTH_MEMCMP(tag, expected, CWIST_SEQ_AUTH_TAG_SIZE) != 0)
        return false;
    pthread_mutex_lock(&ctx->lock);
    bool replayed =
        cache_contains(ctx->nonces, ctx->nonce_count, CWIST_SEQ_AUTH_NONCE_SIZE, nonce) ||
        cache_contains(ctx->completed_ids, ctx->completed_count, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE,
                       message_id);
    if (replayed) {
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    cache_add(ctx->nonces, &ctx->nonce_count, &ctx->nonce_next, ctx->capacity,
              CWIST_SEQ_AUTH_NONCE_SIZE, nonce);
    pthread_mutex_unlock(&ctx->lock);
    memcpy(metadata->message_id, message_id, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    memcpy(metadata->nonce, nonce, CWIST_SEQ_AUTH_NONCE_SIZE);
    *chunk = parsed;
    return true;
}

void cwist_seq_auth_mark_complete(cwist_seq_auth_context_t *ctx,
                                  const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE]) {
    if (!ctx || !message_id) return;
    pthread_mutex_lock(&ctx->lock);
    if (cache_contains(ctx->completed_ids, ctx->completed_count, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE,
                       message_id)) {
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    cache_add(ctx->completed_ids, &ctx->completed_count, &ctx->completed_next, ctx->capacity,
              CWIST_SEQ_AUTH_MESSAGE_ID_SIZE, message_id);
    pthread_mutex_unlock(&ctx->lock);
}
