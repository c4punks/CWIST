/**
 * @file backend_mbedtls.c
 * @brief mbedTLS implementation of the CWIST TLS backend seam.
 *
 * Implements every cwist_mtls_* function declared by the shim headers in
 * src/security/tls/shim/openssl/ on top of mbedTLS (mbedtls_ssl_* and
 * friends).  Call sites keep their OpenSSL-shaped code; the shim macros map
 * it here, so a CWIST_TLS=mbedtls build contains no BoringSSL objects or
 * symbols.
 *
 * Scope and honest limitations (all relative to the BoringSSL backend):
 *  - TLS 1.2 + 1.3, server and client, ALPN, session cache and pre-fork
 *    shared session tickets are fully supported.
 *  - Session-ticket keys are armed deterministically from the CWIST shared
 *    ticket key (carried in SSL_CTX ex_data by https.c), so tickets resume
 *    across forked workers and certificate hot reloads, as with BoringSSL.
 *    TLS 1.3 NewSessionTicket issuance is suppressed, though: mbedTLS writes
 *    it after the handshake wrap-up, which races clients that close right
 *    after their Finished (ECONNRESET on the trailing write fails the whole
 *    handshake).  TLS 1.2 session-ID/ticket resumption via the server cache
 *    keeps working.
 *  - SSL_session_reused() always reports 0: mbedTLS exposes no public
 *    per-connection resumption flag, so the
 *    cwist_https_tls_handshakes_resumed_total() counter stays 0.
 *  - AIA chain autoload (tls_chain.c), ECH and the hybrid PQC group list are
 *    BoringSSL-only; see tls_chain.c / ech.c / pqc_layer.c.
 */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/cipher.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl_ciphersuites.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* --- Error "queue" -------------------------------------------------------- */

static _Thread_local unsigned long cwist_mtls_last_err;

/* Record an mbedTLS error code (negative) for ERR_get_error(). */
static void cwist_mtls_set_err(int ret) {
    cwist_mtls_last_err = (unsigned long)(unsigned int)(-ret);
}

unsigned long cwist_mtls_ERR_get_error(void) {
    unsigned long e = cwist_mtls_last_err;
    cwist_mtls_last_err = 0;
    return e;
}

void cwist_mtls_ERR_error_string_n(unsigned long e, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (e == 0) {
        snprintf(buf, len, "no error");
        return;
    }
    mbedtls_strerror(-(int)e, buf, len);
}

void cwist_mtls_ERR_clear_error(void) {
    cwist_mtls_last_err = 0;
}

/* --- RNG (fork-aware CTR-DRBG) --------------------------------------------- */

static mbedtls_entropy_context cwist_mtls_entropy;
static mbedtls_ctr_drbg_context cwist_mtls_drbg;
static pthread_mutex_t cwist_mtls_rng_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t cwist_mtls_rng_pid;
static int cwist_mtls_rng_ready;

/* Seed (or, after fork, re-seed) the global DRBG. Caller holds the lock. */
static int cwist_mtls_rng_init_locked(void) {
    pid_t pid = getpid();
    if (cwist_mtls_rng_ready && cwist_mtls_rng_pid == pid) return 0;

    if (cwist_mtls_rng_ready) {
        /* Forked worker: the inherited DRBG state would produce streams
         * identical to the parent's, so reseed before generating anything. */
        int ret = mbedtls_ctr_drbg_reseed(&cwist_mtls_drbg, NULL, 0);
        if (ret != 0) {
            cwist_mtls_set_err(ret);
            return -1;
        }
        cwist_mtls_rng_pid = pid;
        return 0;
    }

    mbedtls_entropy_init(&cwist_mtls_entropy);
    mbedtls_ctr_drbg_init(&cwist_mtls_drbg);
    int ret = mbedtls_ctr_drbg_seed(&cwist_mtls_drbg, mbedtls_entropy_func, &cwist_mtls_entropy,
                                    NULL, 0);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        mbedtls_ctr_drbg_free(&cwist_mtls_drbg);
        mbedtls_entropy_free(&cwist_mtls_entropy);
        return -1;
    }
    cwist_mtls_rng_ready = 1;
    cwist_mtls_rng_pid = pid;
    return 0;
}

int cwist_mtls_RAND_bytes(unsigned char *buf, int num) {
    if (!buf || num < 0) return 0;
    pthread_mutex_lock(&cwist_mtls_rng_lock);
    int ok = cwist_mtls_rng_init_locked() == 0 &&
             mbedtls_ctr_drbg_random(&cwist_mtls_drbg, buf, (size_t)num) == 0;
    pthread_mutex_unlock(&cwist_mtls_rng_lock);
    return ok ? 1 : 0;
}

/* f_rng callback for mbedtls_ssl_conf_rng / ticket setup. */
static int cwist_mtls_rng_cb(void *p_rng, unsigned char *buf, size_t len) {
    (void)p_rng;
    if (len > (size_t)INT32_MAX) return -1;
    return cwist_mtls_RAND_bytes(buf, (int)len) == 1 ? 0 : -1;
}

/* --- Secure memory helpers -------------------------------------------------- */

void cwist_mtls_OPENSSL_cleanse(void *ptr, size_t len) {
    mbedtls_platform_zeroize(ptr, len);
}

int cwist_mtls_CRYPTO_memcmp(const void *a, const void *b, size_t len) {
    const volatile unsigned char *pa = (const volatile unsigned char *)a;
    const volatile unsigned char *pb = (const volatile unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) diff |= pa[i] ^ pb[i];
    return diff;
}

/* --- Digests / HMAC ---------------------------------------------------------- */

struct cwist_mtls_evp_md {
    mbedtls_md_type_t type;
};

static const struct cwist_mtls_evp_md cwist_mtls_md_sha256 = {MBEDTLS_MD_SHA256};

const EVP_MD *cwist_mtls_EVP_sha256(void) {
    return &cwist_mtls_md_sha256;
}

unsigned char *cwist_mtls_HMAC(const EVP_MD *md, const void *key, int key_len,
                               const unsigned char *msg, size_t msg_len, unsigned char *out,
                               unsigned int *out_len) {
    if (!md || !out) return NULL;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md->type);
    if (!info) return NULL;
    int ret = mbedtls_md_hmac(info, (const unsigned char *)key, (size_t)key_len, msg, msg_len, out);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        return NULL;
    }
    if (out_len) *out_len = mbedtls_md_get_size(info);
    return out;
}

struct cwist_mtls_hmac_ctx {
    mbedtls_md_context_t md;
    int started;
};

HMAC_CTX *cwist_mtls_HMAC_CTX_new(void) {
    HMAC_CTX *ctx = (HMAC_CTX *)calloc(1, sizeof(*ctx));
    if (ctx) mbedtls_md_init(&ctx->md);
    return ctx;
}

void cwist_mtls_HMAC_CTX_free(HMAC_CTX *ctx) {
    if (!ctx) return;
    mbedtls_md_free(&ctx->md);
    free(ctx);
}

int cwist_mtls_HMAC_Init_ex(HMAC_CTX *ctx, const void *key, int key_len, const EVP_MD *md,
                            void *engine) {
    (void)engine;
    if (!ctx || !md) return 0;
    mbedtls_md_free(&ctx->md);
    mbedtls_md_init(&ctx->md);
    ctx->started = 0;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md->type);
    if (!info) return 0;
    if (mbedtls_md_setup(&ctx->md, info, 1) != 0) return 0;
    if (mbedtls_md_hmac_starts(&ctx->md, (const unsigned char *)key, (size_t)key_len) != 0) {
        return 0;
    }
    ctx->started = 1;
    return 1;
}

int cwist_mtls_HMAC_Update(HMAC_CTX *ctx, const unsigned char *data, size_t len) {
    if (!ctx || !ctx->started) return 0;
    return mbedtls_md_hmac_update(&ctx->md, data, len) == 0 ? 1 : 0;
}

int cwist_mtls_HMAC_Final(HMAC_CTX *ctx, unsigned char *out, unsigned int *out_len) {
    if (!ctx || !ctx->started || !out) return 0;
    if (mbedtls_md_hmac_finish(&ctx->md, out) != 0) return 0;
    if (out_len) *out_len = mbedtls_md_get_size(mbedtls_md_info_from_ctx(&ctx->md));
    ctx->started = 0;
    return 1;
}

/* --- AES-256-CBC (EVP subset) ------------------------------------------------ */

struct cwist_mtls_cipher {
    mbedtls_cipher_type_t type;
};

static const struct cwist_mtls_cipher cwist_mtls_cipher_aes_256_cbc = {MBEDTLS_CIPHER_AES_256_CBC};

const EVP_CIPHER *cwist_mtls_EVP_aes_256_cbc(void) {
    return &cwist_mtls_cipher_aes_256_cbc;
}

struct cwist_mtls_cipher_ctx {
    mbedtls_cipher_context_t cipher;
};

EVP_CIPHER_CTX *cwist_mtls_EVP_CIPHER_CTX_new(void) {
    EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX *)calloc(1, sizeof(*ctx));
    if (ctx) mbedtls_cipher_init(&ctx->cipher);
    return ctx;
}

void cwist_mtls_EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx) {
    if (!ctx) return;
    mbedtls_cipher_free(&ctx->cipher);
    free(ctx);
}

static int cwist_mtls_cipher_init(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type,
                                  const unsigned char *key, const unsigned char *iv,
                                  mbedtls_operation_t op) {
    if (!ctx || !type || !key || !iv) return 0;
    mbedtls_cipher_free(&ctx->cipher);
    mbedtls_cipher_init(&ctx->cipher);
    const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(type->type);
    if (!info) return 0;
    /* PKCS#7 padding, matching the EVP default. */
    if (mbedtls_cipher_setup(&ctx->cipher, info) != 0 ||
        mbedtls_cipher_setkey(&ctx->cipher, key, 256, op) != 0 ||
        mbedtls_cipher_set_padding_mode(&ctx->cipher, MBEDTLS_PADDING_PKCS7) != 0 ||
        mbedtls_cipher_set_iv(&ctx->cipher, iv, 16) != 0 ||
        mbedtls_cipher_reset(&ctx->cipher) != 0) {
        return 0;
    }
    return 1;
}

int cwist_mtls_EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, void *engine,
                                  const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    return cwist_mtls_cipher_init(ctx, type, key, iv, MBEDTLS_ENCRYPT);
}

int cwist_mtls_EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, void *engine,
                                  const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    return cwist_mtls_cipher_init(ctx, type, key, iv, MBEDTLS_DECRYPT);
}

int cwist_mtls_EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                                 const unsigned char *in, int inl) {
    size_t olen = 0;
    if (!ctx || !outl || inl < 0) return 0;
    if (mbedtls_cipher_update(&ctx->cipher, in, (size_t)inl, out, &olen) != 0) return 0;
    *outl = (int)olen;
    return 1;
}

int cwist_mtls_EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl) {
    size_t olen = 0;
    if (!ctx || !outl) return 0;
    if (mbedtls_cipher_finish(&ctx->cipher, out, &olen) != 0) return 0;
    *outl = (int)olen;
    return 1;
}

int cwist_mtls_EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                                 const unsigned char *in, int inl) {
    return cwist_mtls_EVP_EncryptUpdate(ctx, out, outl, in, inl);
}

int cwist_mtls_EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl) {
    return cwist_mtls_EVP_EncryptFinal_ex(ctx, out, outl);
}

/* --- ALPN ------------------------------------------------------------------- */

int cwist_mtls_SSL_select_next_proto(unsigned char **out, unsigned char *outlen,
                                     const unsigned char *server, unsigned int server_len,
                                     const unsigned char *client, unsigned int client_len) {
    /* Server-preference scan, same contract as OpenSSL. */
    for (unsigned int i = 0; i < server_len;) {
        unsigned int slen = server[i++];
        if (i + slen > server_len) break;
        for (unsigned int j = 0; j < client_len;) {
            unsigned int clen = client[j++];
            if (j + clen > client_len) return 0;
            if (clen == slen && memcmp(server + i, client + j, slen) == 0) {
                *out = (unsigned char *)(server + i);
                *outlen = (unsigned char)slen;
                return OPENSSL_NPN_NEGOTIATED;
            }
            j += clen;
        }
        i += slen;
    }
    return 0;
}

/* --- SSL_CTX ----------------------------------------------------------------- */

struct cwist_mtls_method {
    int is_server;
};

static const struct cwist_mtls_method cwist_mtls_server_method = {1};
static const struct cwist_mtls_method cwist_mtls_client_method = {0};

const SSL_METHOD *cwist_mtls_TLS_server_method(void) {
    return &cwist_mtls_server_method;
}

const SSL_METHOD *cwist_mtls_TLS_client_method(void) {
    return &cwist_mtls_client_method;
}

#define CWIST_MTLS_EX_DATA_MAX 8

struct cwist_mtls_ctx {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;    /* own certificate chain (server) */
    mbedtls_pk_context pkey;  /* own private key (server) */
    mbedtls_x509_crt ca_chain; /* trust store (client) */
    mbedtls_ssl_cache_context cache;
    mbedtls_ssl_ticket_context *ticket; /* armed on SSL_CTX_set_tlsext_ticket_key_cb */
    int is_server;
    int cert_loaded;
    int key_loaded;
    unsigned long options;
    int min_version;              /* MBEDTLS_SSL_VERSION_* or 0 */
    long sess_cache_max;          /* 0 = leave the mbedTLS default */
    long timeout_sec;             /* session/ticket lifetime, 0 = default */
    /* ALPN: static list installed on the conf. Storage lives here because
     * mbedtls_ssl_conf_alpn_protocols() does not copy the list. */
    const char *alpn_list[3];
    char alpn_names[2][16]; /* name storage for the client-side list */
    int alpn_count;
    /* Ciphersuites: OpenSSL-style names split by TLS version, merged on
     * apply (mbedtls_ssl_conf_ciphersuites takes one unified list). */
    int cs12[16];
    size_t cs12_count;
    int cs13[8];
    size_t cs13_count;
    int cs_applied[32];
    void *ex_data[CWIST_MTLS_EX_DATA_MAX];
    _Atomic int refs;
};

/* ex_data indexes, allocated lazily exactly like SSL_CTX_get_ex_new_index. */
static _Atomic int cwist_mtls_ex_data_next;

SSL_CTX *cwist_mtls_SSL_CTX_new(const SSL_METHOD *method) {
    if (!method) return NULL;
    SSL_CTX *ctx = (SSL_CTX *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->is_server = method->is_server;
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_pk_init(&ctx->pkey);
    mbedtls_x509_crt_init(&ctx->ca_chain);
    mbedtls_ssl_cache_init(&ctx->cache);

    if (mbedtls_ssl_config_defaults(&ctx->conf,
                                    ctx->is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        mbedtls_ssl_cache_free(&ctx->cache);
        mbedtls_x509_crt_free(&ctx->ca_chain);
        mbedtls_pk_free(&ctx->pkey);
        mbedtls_x509_crt_free(&ctx->cert);
        mbedtls_ssl_config_free(&ctx->conf);
        free(ctx);
        return NULL;
    }

    mbedtls_ssl_conf_rng(&ctx->conf, cwist_mtls_rng_cb, NULL);
    /* CWIST does not do client-certificate authentication. */
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    if (ctx->is_server) {
        mbedtls_ssl_conf_session_cache(&ctx->conf, &ctx->cache, mbedtls_ssl_cache_get,
                                       mbedtls_ssl_cache_set);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
        /* mbedTLS writes the TLS 1.3 NewSessionTicket after handshake
         * wrap-up (a trailing write BoringSSL folds into its second flight).
         * A client that closes right after its Finished - routine for the
         * short-lived probes in the CWIST test suite - resets the socket and
         * that trailing write then fails the whole handshake with
         * ECONNRESET.  Suppress server-side NST: TLS 1.3 ticket resumption
         * is off, TLS 1.2 session-ID resumption via the server cache above
         * keeps working. */
        mbedtls_ssl_conf_new_session_tickets(&ctx->conf, 0);
#endif
    }
    atomic_init(&ctx->refs, 1);
    return ctx;
}

/* Drop the last reference: free the mbedTLS contexts the conf points at. */
static void cwist_mtls_ctx_destroy(SSL_CTX *ctx) {
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_x509_crt_free(&ctx->ca_chain);
    mbedtls_ssl_cache_free(&ctx->cache);
    if (ctx->ticket) {
        mbedtls_ssl_ticket_free(ctx->ticket);
        free(ctx->ticket);
    }
    free(ctx);
}

void cwist_mtls_SSL_CTX_free(SSL_CTX *ctx) {
    if (!ctx) return;
    if (atomic_fetch_sub_explicit(&ctx->refs, 1, memory_order_acq_rel) == 1) {
        cwist_mtls_ctx_destroy(ctx);
    }
}

int cwist_mtls_SSL_CTX_set_min_proto_version(SSL_CTX *ctx, uint16_t version) {
    if (!ctx) return 0;
    if (version == TLS1_2_VERSION) {
        ctx->min_version = MBEDTLS_SSL_VERSION_TLS1_2;
    } else if (version == TLS1_3_VERSION) {
        ctx->min_version = MBEDTLS_SSL_VERSION_TLS1_3;
    } else {
        return 0;
    }
    mbedtls_ssl_conf_min_tls_version(&ctx->conf, (mbedtls_ssl_protocol_version)ctx->min_version);
    return 1;
}

long cwist_mtls_SSL_CTX_set_options(SSL_CTX *ctx, unsigned long options) {
    if (!ctx) return 0;
    ctx->options |= options;
    if (options & SSL_OP_NO_TICKET) {
        mbedtls_ssl_conf_session_tickets(&ctx->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    }
    if (options & SSL_OP_NO_TLSv1_2 && ctx->min_version < MBEDTLS_SSL_VERSION_TLS1_3) {
        ctx->min_version = MBEDTLS_SSL_VERSION_TLS1_3;
        mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    }
    /* NO_COMPRESSION / NO_RENEGOTIATION / CIPHER_SERVER_PREFERENCE match the
     * mbedTLS defaults (no compression support, renegotiation disabled,
     * server-order ciphersuite pick), so they need no action. */
    return (long)ctx->options;
}

long cwist_mtls_SSL_CTX_set_mode(SSL_CTX *ctx, long mode) {
    (void)mode; /* AUTO_RETRY is the only mode mbedTLS has. */
    (void)ctx;
    return SSL_MODE_AUTO_RETRY;
}

/* OpenSSL cipher name -> mbedTLS ciphersuite IANA id, for the fixed profile
 * https.c applies. Unknown names are skipped by the caller. */
static int cwist_mtls_cipher_id(const char *name, size_t len, int *is_tls13) {
    struct entry {
        const char *name;
        int id;
        int tls13;
    };
    static const struct entry table[] = {
        {"ECDHE-ECDSA-AES128-GCM-SHA256", MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0},
        {"ECDHE-RSA-AES128-GCM-SHA256", MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0},
        {"ECDHE-ECDSA-AES256-GCM-SHA384", MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 0},
        {"ECDHE-RSA-AES256-GCM-SHA384", MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 0},
        {"ECDHE-ECDSA-CHACHA20-POLY1305", MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, 0},
        {"ECDHE-RSA-CHACHA20-POLY1305", MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, 0},
        {"TLS_AES_128_GCM_SHA256", MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 1},
        {"TLS_AES_256_GCM_SHA384", MBEDTLS_TLS1_3_AES_256_GCM_SHA384, 1},
        {"TLS_CHACHA20_POLY1305_SHA256", MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, 1},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) == len && memcmp(table[i].name, name, len) == 0) {
            *is_tls13 = table[i].tls13;
            return table[i].id;
        }
    }
    return -1;
}

/* Rebuild and install the unified ciphersuite list (TLS 1.3 first, then the
 * TLS 1.2 picks, matching the server-preference order https.c requests). */
static int cwist_mtls_apply_ciphersuites(SSL_CTX *ctx) {
    size_t n = 0;
    for (size_t i = 0; i < ctx->cs13_count && n < 30; i++) ctx->cs_applied[n++] = ctx->cs13[i];
    for (size_t i = 0; i < ctx->cs12_count && n < 30; i++) ctx->cs_applied[n++] = ctx->cs12[i];
    if (n == 0) return 0;
    ctx->cs_applied[n] = 0;
    mbedtls_ssl_conf_ciphersuites(&ctx->conf, ctx->cs_applied);
    return 1;
}

static int cwist_mtls_set_cipher_list(SSL_CTX *ctx, const char *list, int want_tls13) {
    if (!ctx || !list) return 0;
    size_t recognized = 0;
    const char *p = list;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        int is_tls13 = 0;
        int id = cwist_mtls_cipher_id(p, len, &is_tls13);
        if (id >= 0) {
            if (is_tls13 && ctx->cs13_count < 8) {
                ctx->cs13[ctx->cs13_count++] = id;
                recognized++;
            } else if (!is_tls13 && ctx->cs12_count < 16) {
                ctx->cs12[ctx->cs12_count++] = id;
                recognized++;
            }
        }
        /* Unmapped names are ignored: OpenSSL would accept any name it
         * knows, we only carry the ones CWIST's profiles use. */
        if (!colon) break;
        p = colon + 1;
    }
    if (recognized == 0) return 0; /* mirror OpenSSL's all-unknown failure */
    return want_tls13 ? 1 : cwist_mtls_apply_ciphersuites(ctx);
}

int cwist_mtls_SSL_CTX_set_cipher_list(SSL_CTX *ctx, const char *list) {
    return cwist_mtls_set_cipher_list(ctx, list, 0);
}

int cwist_mtls_SSL_CTX_set_ciphersuites(SSL_CTX *ctx, const char *list) {
    return cwist_mtls_set_cipher_list(ctx, list, 1) ? cwist_mtls_apply_ciphersuites(ctx) : 0;
}

/* Install cert+key on the conf once both are loaded. */
static int cwist_mtls_maybe_conf_own_cert(SSL_CTX *ctx) {
    if (!ctx->cert_loaded || !ctx->key_loaded) return 1;
    int ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        return 0;
    }
    return 1;
}

int cwist_mtls_SSL_CTX_use_certificate_chain_file(SSL_CTX *ctx, const char *path) {
    if (!ctx || !path) return 0;
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_x509_crt_init(&ctx->cert);
    int ret = mbedtls_x509_crt_parse_file(&ctx->cert, path);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        return 0;
    }
    ctx->cert_loaded = 1;
    return cwist_mtls_maybe_conf_own_cert(ctx);
}

int cwist_mtls_SSL_CTX_use_PrivateKey_file(SSL_CTX *ctx, const char *path, int type) {
    if (!ctx || !path || type != SSL_FILETYPE_PEM) return 0;
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_pk_init(&ctx->pkey);
    int ret = mbedtls_pk_parse_keyfile(&ctx->pkey, path, NULL, cwist_mtls_rng_cb, NULL);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        return 0;
    }
    ctx->key_loaded = 1;
    return cwist_mtls_maybe_conf_own_cert(ctx);
}

int cwist_mtls_SSL_CTX_use_certificate_file(SSL_CTX *ctx, const char *path, int type) {
    (void)type; /* PEM only, same restriction as use_PrivateKey_file */
    return cwist_mtls_SSL_CTX_use_certificate_chain_file(ctx, path);
}

int cwist_mtls_SSL_CTX_get_min_proto_version(const SSL_CTX *ctx) {
    if (!ctx || ctx->min_version == 0) return 0;
    return ctx->min_version == MBEDTLS_SSL_VERSION_TLS1_3 ? TLS1_3_VERSION : TLS1_2_VERSION;
}

int cwist_mtls_SSL_CTX_check_private_key(const SSL_CTX *ctx) {
    if (!ctx || !ctx->cert_loaded || !ctx->key_loaded) return 0;
    return mbedtls_pk_check_pair(&ctx->cert.pk, &ctx->pkey, cwist_mtls_rng_cb, NULL) == 0 ? 1 : 0;
}

long cwist_mtls_SSL_CTX_set_session_cache_mode(SSL_CTX *ctx, long mode) {
    (void)ctx;
    (void)mode; /* the server cache is always configured (see SSL_CTX_new) */
    return SSL_SESS_CACHE_SERVER;
}

long cwist_mtls_SSL_CTX_sess_set_cache_size(SSL_CTX *ctx, unsigned long size) {
    if (!ctx) return 0;
    ctx->sess_cache_max = (long)size;
    if (size > 0) mbedtls_ssl_cache_set_max_entries(&ctx->cache, (int)size);
    return ctx->sess_cache_max;
}

int cwist_mtls_SSL_CTX_set_session_id_context(SSL_CTX *ctx, const unsigned char *sid_ctx,
                                              unsigned int sid_ctx_len) {
    /* mbedTLS needs no session-id context to resume from its server cache. */
    (void)ctx;
    (void)sid_ctx;
    (void)sid_ctx_len;
    return 1;
}

long cwist_mtls_SSL_CTX_set_timeout(SSL_CTX *ctx, long seconds) {
    if (!ctx) return 0;
    ctx->timeout_sec = seconds;
    mbedtls_ssl_cache_set_timeout(&ctx->cache, (int)seconds);
    return seconds;
}

void cwist_mtls_SSL_CTX_set_alpn_select_cb(
    SSL_CTX *ctx,
    int (*cb)(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
              unsigned int inlen, void *arg),
    void *arg) {
    if (!ctx || !cb) return;

    /* mbedTLS has no server ALPN callback; it picks the first conf-list
     * protocol the client offered.  CWIST's callback (https.c) is a pure
     * function of the context's HTTP/2 flag, which is final by the time the
     * callback is installed (init options / reload), so evaluate it once
     * here against a synthetic offer of every TCP-TLS protocol CWIST
     * supports and install the implied server-preference list statically.
     * ssl is NULL: CWIST's callback never dereferences it. */
    static const unsigned char offer[] = "\x02h2\x08http/1.1";
    static char proto_h2[] = "h2";
    static char proto_http11[] = "http/1.1";
    const unsigned char *sel = NULL;
    unsigned char sel_len = 0;
    if (cb(NULL, &sel, &sel_len, offer, (unsigned int)sizeof(offer) - 1, arg) !=
            SSL_TLSEXT_ERR_OK ||
        !sel || sel_len == 0) {
        return; /* NOACK: no ALPN configured, exactly the OpenSSL outcome */
    }

    int n = 0;
    if (sel_len == 2 && memcmp(sel, "h2", 2) == 0) {
        ctx->alpn_list[n++] = proto_h2;
        ctx->alpn_list[n++] = proto_http11;
    } else if (sel_len == 8 && memcmp(sel, "http/1.1", 8) == 0) {
        ctx->alpn_list[n++] = proto_http11;
    } else {
        return; /* unknown protocol: leave ALPN off rather than guess */
    }
    ctx->alpn_list[n] = NULL;
    ctx->alpn_count = n;
    mbedtls_ssl_conf_alpn_protocols(&ctx->conf, ctx->alpn_list);
}

int cwist_mtls_SSL_CTX_set_alpn_protos(SSL_CTX *ctx, const unsigned char *protos,
                                       unsigned int protos_len) {
    /* Client side: parse the wire-format list into a NUL-terminated string
     * array stored on the ctx (mbedTLS does not copy the list). */
    if (!ctx || (!protos && protos_len > 0)) return -1;
    ctx->alpn_count = 0;
    ctx->alpn_list[0] = NULL;
    if (protos_len == 0) {
        mbedtls_ssl_conf_alpn_protocols(&ctx->conf, NULL);
        return 0;
    }

    unsigned int i = 0;
    int n = 0;
    while (i < protos_len) {
        unsigned int len = protos[i++];
        if (i + len > protos_len || len == 0 || len >= sizeof(ctx->alpn_names[0]) || n >= 2) {
            return -1;
        }
        memcpy(ctx->alpn_names[n], protos + i, len);
        ctx->alpn_names[n][len] = '\0';
        ctx->alpn_list[n] = ctx->alpn_names[n];
        n++;
        i += len;
    }
    ctx->alpn_list[n] = NULL;
    ctx->alpn_count = n;
    if (mbedtls_ssl_conf_alpn_protocols(&ctx->conf, ctx->alpn_list) != 0) {
        return -1;
    }
    return 0;
}

/* Mirrors cwist_tls_ticket_key in src/net/http/https.c: the shared session
 * ticket key https.c generates pre-fork and stores as SSL_CTX ex_data. */
struct cwist_mtls_ticket_key {
    unsigned char name[16];
    unsigned char aes_key[32];
    unsigned char hmac_key[32];
};

void cwist_mtls_SSL_CTX_set_tlsext_ticket_key_cb(
    SSL_CTX *ctx,
    int (*cb)(SSL *ssl, unsigned char *key_name, unsigned char *iv, EVP_CIPHER_CTX *ectx,
              HMAC_CTX *hctx, int enc)) {
    (void)cb; /* the mbedTLS ticket module replaces the callback mechanism */
    if (!ctx || !ctx->is_server) return;
    if (ctx->options & SSL_OP_NO_TICKET) {
        mbedtls_ssl_conf_session_tickets(&ctx->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
        return;
    }

    /* The shared key is always installed as ex_data before this call (both
     * the init and the reload path in https.c).  Using it - rather than the
     * random key mbedtls_ssl_ticket_setup() would mint - keeps tickets
     * resumable across certificate hot reloads; all forked workers inherit
     * the same material because the parent builds the context pre-fork. */
    const struct cwist_mtls_ticket_key *key = NULL;
    for (int i = 0; i < CWIST_MTLS_EX_DATA_MAX; i++) {
        if (ctx->ex_data[i]) {
            key = (const struct cwist_mtls_ticket_key *)ctx->ex_data[i];
            break;
        }
    }
    if (!key) return; /* no shared key: leave tickets unconfigured */

    mbedtls_ssl_ticket_context *t = (mbedtls_ssl_ticket_context *)calloc(1, sizeof(*t));
    if (!t) return;
    mbedtls_ssl_ticket_init(t);
    uint32_t lifetime = ctx->timeout_sec > 0 ? (uint32_t)ctx->timeout_sec : 300;
    int ret = mbedtls_ssl_ticket_setup(t, cwist_mtls_rng_cb, NULL, MBEDTLS_CIPHER_AES_256_GCM,
                                       lifetime);
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        mbedtls_ssl_ticket_free(t);
        free(t);
        return;
    }

    /* Re-key slot 0 deterministically: same key name and AES-256-GCM key on
     * every worker and every reload, so a ticket issued anywhere decrypts
     * everywhere (mirrors the BoringSSL STEK callback). */
    mbedtls_cipher_free(&t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(ctx));
    mbedtls_cipher_init(&t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(ctx));
    ret = mbedtls_cipher_setup(&t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(ctx),
                               mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_GCM));
    if (ret == 0) {
        ret = mbedtls_cipher_setkey(&t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(ctx), key->aes_key,
                                    256, MBEDTLS_ENCRYPT);
    }
    if (ret != 0) {
        cwist_mtls_set_err(ret);
        mbedtls_ssl_ticket_free(t);
        free(t);
        return;
    }
    memcpy(t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(name), key->name,
           MBEDTLS_SSL_TICKET_KEY_NAME_BYTES);
    t->MBEDTLS_PRIVATE(keys)[0].MBEDTLS_PRIVATE(lifetime) = lifetime;
    t->MBEDTLS_PRIVATE(active) = 0;

    mbedtls_ssl_conf_session_tickets_cb(&ctx->conf, mbedtls_ssl_ticket_write,
                                        mbedtls_ssl_ticket_parse, t);
    if (ctx->ticket) {
        mbedtls_ssl_ticket_free(ctx->ticket);
        free(ctx->ticket);
    }
    ctx->ticket = t;
}

int cwist_mtls_SSL_CTX_get_ex_new_index(long argl, void *argp, void *new_func, void *dup_func,
                                        void *free_func) {
    (void)argl;
    (void)argp;
    (void)new_func;
    (void)dup_func;
    (void)free_func;
    int idx = atomic_fetch_add_explicit(&cwist_mtls_ex_data_next, 1, memory_order_relaxed);
    return idx < CWIST_MTLS_EX_DATA_MAX ? idx : -1;
}

int cwist_mtls_SSL_CTX_set_ex_data(SSL_CTX *ctx, int idx, void *data) {
    if (!ctx || idx < 0 || idx >= CWIST_MTLS_EX_DATA_MAX) return 0;
    ctx->ex_data[idx] = data;
    return 1;
}

void *cwist_mtls_SSL_CTX_get_ex_data(const SSL_CTX *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= CWIST_MTLS_EX_DATA_MAX) return NULL;
    return ctx->ex_data[idx];
}

void cwist_mtls_SSL_CTX_set_verify(SSL_CTX *ctx, int mode, void *callback) {
    (void)callback;
    if (!ctx) return;
    mbedtls_ssl_conf_authmode(&ctx->conf, (mode & SSL_VERIFY_PEER) ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                                   : MBEDTLS_SSL_VERIFY_NONE);
}

int cwist_mtls_SSL_CTX_set_default_verify_paths(SSL_CTX *ctx) {
    if (!ctx) return 0;
    int loaded = 0;
    const char *file = getenv("SSL_CERT_FILE");
    if (!file || !*file) file = "/etc/ssl/certs/ca-certificates.crt";
    if (mbedtls_x509_crt_parse_file(&ctx->ca_chain, file) >= 0) loaded = 1;
    const char *dir = getenv("SSL_CERT_DIR");
    if (!dir || !*dir) dir = "/etc/ssl/certs";
    if (mbedtls_x509_crt_parse_path(&ctx->ca_chain, dir) >= 0) loaded = 1;
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain, NULL);
    (void)loaded; /* an empty trust store fails at verify time, not here */
    return 1;
}

int cwist_mtls_SSL_CTX_set1_groups_list(SSL_CTX *ctx, const char *groups) {
    /* Named-group configuration is not surfaced: mbedTLS 3.6 negotiates its
     * compiled-in curves (x25519, secp256r1, ...), which covers everything
     * CWIST's default list asks for except the BoringSSL-only hybrid PQC
     * groups (handled in pqc_layer.c). */
    (void)ctx;
    (void)groups;
    return 1;
}

/* --- SSL (per-connection) ---------------------------------------------------- */

struct cwist_mtls_ssl {
    mbedtls_ssl_context ssl;
    /* Shallow copy of the ctx's config, taken at SSL_new: lets per-connection
     * tweaks (client-side SSL_set_alpn_protos) work and insulates the
     * connection from later conf mutations.  All pointers in it refer to
     * ctx-owned objects kept alive by the reference below. */
    mbedtls_ssl_config conf;
    const char *alpn_list[3];
    char alpn_names[2][16];
    mbedtls_net_context net;
    SSL_CTX *ctx; /* reference held for the connection's lifetime */
    int last_err; /* SSL_ERROR_* from the last failing operation */
    int fd;
};

SSL *cwist_mtls_SSL_new(SSL_CTX *ctx) {
    if (!ctx) return NULL;
    SSL *s = (SSL *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_net_init(&s->net);
    s->conf = ctx->conf;
    if (mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) {
        mbedtls_net_free(&s->net);
        mbedtls_ssl_free(&s->ssl);
        free(s);
        return NULL;
    }
    atomic_fetch_add_explicit(&ctx->refs, 1, memory_order_relaxed);
    s->ctx = ctx;
    s->last_err = SSL_ERROR_NONE;
    s->fd = -1;
    return s;
}

void cwist_mtls_SSL_free(SSL *ssl) {
    if (!ssl) return;
    SSL_CTX *ctx = ssl->ctx;
    mbedtls_ssl_free(&ssl->ssl);
    mbedtls_net_free(&ssl->net);
    free(ssl);
    if (ctx) cwist_mtls_SSL_CTX_free(ctx);
}

int cwist_mtls_SSL_set_fd(SSL *ssl, int fd) {
    if (!ssl) return 0;
    ssl->fd = fd;
    ssl->net.fd = fd;
    /* Non-blocking sockets surface as WANT_READ/WANT_WRITE through
     * net_sockets' EAGAIN mapping, matching BoringSSL semantics. */
    mbedtls_ssl_set_bio(&ssl->ssl, &ssl->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    return 1;
}

/* Map one mbedtls_ssl_* return code to the OpenSSL (ret, SSL_get_error)
 * convention: 1 on success, 0 on clean close, -1 otherwise. */
static int cwist_mtls_map_result(SSL *ssl, int ret, int ok_value) {
    if (ret == ok_value) {
        ssl->last_err = SSL_ERROR_NONE;
        return 1;
    }
    switch (ret) {
        case MBEDTLS_ERR_SSL_WANT_READ:
            ssl->last_err = SSL_ERROR_WANT_READ;
            return -1;
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            ssl->last_err = SSL_ERROR_WANT_WRITE;
            return -1;
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            ssl->last_err = SSL_ERROR_ZERO_RETURN;
            return 0;
        default:
            cwist_mtls_set_err(ret);
            ssl->last_err = SSL_ERROR_SSL;
            return -1;
    }
}

static int cwist_mtls_handshake(SSL *ssl) {
    return cwist_mtls_map_result(ssl, mbedtls_ssl_handshake(&ssl->ssl), 0);
}

int cwist_mtls_SSL_accept(SSL *ssl) {
    if (!ssl) return -1;
    return cwist_mtls_handshake(ssl);
}

int cwist_mtls_SSL_connect(SSL *ssl) {
    if (!ssl) return -1;
    return cwist_mtls_handshake(ssl);
}

int cwist_mtls_SSL_read(SSL *ssl, void *buf, int num) {
    if (!ssl || !buf || num <= 0) return -1;
    int ret;
    do {
        ret = mbedtls_ssl_read(&ssl->ssl, (unsigned char *)buf, (size_t)num);
        /* TLS 1.3 post-handshake NewSessionTicket: consumed internally,
         * retry the read instead of surfacing it (OpenSSL does the same). */
    } while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET);
    if (ret > 0) {
        ssl->last_err = SSL_ERROR_NONE;
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
        ssl->last_err = SSL_ERROR_ZERO_RETURN;
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        ssl->last_err = SSL_ERROR_WANT_READ;
        return -1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        ssl->last_err = SSL_ERROR_WANT_WRITE;
        return -1;
    }
    cwist_mtls_set_err(ret);
    ssl->last_err = SSL_ERROR_SSL;
    return -1;
}

int cwist_mtls_SSL_write(SSL *ssl, const void *buf, int num) {
    if (!ssl || !buf || num < 0) return -1;
    int ret = mbedtls_ssl_write(&ssl->ssl, (const unsigned char *)buf, (size_t)num);
    if (ret > 0) {
        ssl->last_err = SSL_ERROR_NONE;
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        ssl->last_err = SSL_ERROR_WANT_READ;
        return -1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        ssl->last_err = SSL_ERROR_WANT_WRITE;
        return -1;
    }
    cwist_mtls_set_err(ret);
    ssl->last_err = SSL_ERROR_SSL;
    return -1;
}

int cwist_mtls_SSL_pending(const SSL *ssl) {
    if (!ssl) return 0;
    return (int)mbedtls_ssl_get_bytes_avail(&ssl->ssl);
}

int cwist_mtls_SSL_shutdown(SSL *ssl) {
    if (!ssl) return 0;
    /* Best-effort close_notify, like the OpenSSL unidirectional shutdown. */
    (void)mbedtls_ssl_close_notify(&ssl->ssl);
    return 1;
}

int cwist_mtls_SSL_get_error(const SSL *ssl, int ret) {
    (void)ret;
    if (!ssl) return SSL_ERROR_SSL;
    return ssl->last_err;
}

SSL_CTX *cwist_mtls_SSL_get_SSL_CTX(const SSL *ssl) {
    return ssl ? ssl->ctx : NULL;
}

int cwist_mtls_SSL_session_reused(const SSL *ssl) {
    /* mbedTLS exposes no public per-connection resumption flag; the
     * resumed-handshake counter stays 0 on this backend. */
    (void)ssl;
    return 0;
}

int cwist_mtls_SSL_version(const SSL *ssl) {
    if (!ssl) return 0;
    const char *v = mbedtls_ssl_get_version(&ssl->ssl);
    if (v && strcmp(v, "TLSv1.3") == 0) return TLS1_3_VERSION;
    if (v && strcmp(v, "TLSv1.2") == 0) return TLS1_2_VERSION;
    return 0;
}

const char *cwist_mtls_SSL_get_cipher_name(const SSL *ssl) {
    if (!ssl) return NULL;
    int id = mbedtls_ssl_get_ciphersuite_id_from_ssl(&ssl->ssl);
    /* Report OpenSSL-style names so the observability buckets in https.c
     * classify the same way as on the BoringSSL backend. */
    switch (id) {
        case MBEDTLS_TLS1_3_AES_128_GCM_SHA256: return "TLS_AES_128_GCM_SHA256";
        case MBEDTLS_TLS1_3_AES_256_GCM_SHA384: return "TLS_AES_256_GCM_SHA384";
        case MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256: return "TLS_CHACHA20_POLY1305_SHA256";
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
        case MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: return "ECDHE-RSA-AES128-GCM-SHA256";
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
        case MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: return "ECDHE-RSA-AES256-GCM-SHA384";
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256:
            return "ECDHE-ECDSA-CHACHA20-POLY1305";
        case MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256:
            return "ECDHE-RSA-CHACHA20-POLY1305";
        default: return mbedtls_ssl_get_ciphersuite(&ssl->ssl);
    }
}

void cwist_mtls_SSL_get0_alpn_selected(const SSL *ssl, const unsigned char **data,
                                       unsigned int *len) {
    const char *p = ssl ? mbedtls_ssl_get_alpn_protocol(&ssl->ssl) : NULL;
    *data = (const unsigned char *)p;
    *len = p ? (unsigned int)strlen(p) : 0;
}

int cwist_mtls_SSL_set_tlsext_host_name(SSL *ssl, const char *name) {
    if (!ssl || !name) return 0;
    return mbedtls_ssl_set_hostname(&ssl->ssl, name) == 0 ? 1 : 0;
}

int cwist_mtls_SSL_set_alpn_protos(SSL *ssl, const unsigned char *protos, unsigned int protos_len) {
    /* Client-side per-connection ALPN (OpenSSL returns 0 on success). */
    if (!ssl || (!protos && protos_len > 0)) return -1;
    if (protos_len == 0) {
        mbedtls_ssl_conf_alpn_protocols(&ssl->conf, NULL);
        return 0;
    }
    unsigned int i = 0;
    int n = 0;
    while (i < protos_len) {
        unsigned int len = protos[i++];
        if (i + len > protos_len || len == 0 || len >= sizeof(ssl->alpn_names[0]) || n >= 2) {
            return -1;
        }
        memcpy(ssl->alpn_names[n], protos + i, len);
        ssl->alpn_names[n][len] = '\0';
        ssl->alpn_list[n] = ssl->alpn_names[n];
        n++;
        i += len;
    }
    ssl->alpn_list[n] = NULL;
    return mbedtls_ssl_conf_alpn_protocols(&ssl->conf, ssl->alpn_list) == 0 ? 0 : -1;
}
