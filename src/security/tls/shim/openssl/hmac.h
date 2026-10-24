/**
 * @file shim/openssl/hmac.h
 * @brief mbedTLS TLS backend: HMAC subset (HMAC-SHA256, one-shot + streaming).
 *
 * Part of the CWIST TLS backend seam; see shim/openssl/ssl.h for the design.
 * Backs signed session cookies (session.c), sequenced-chunk authentication
 * (seq_auth.c) and JWT HS256.
 */
#ifndef CWIST_SHIM_OPENSSL_HMAC_H
#define CWIST_SHIM_OPENSSL_HMAC_H

#include <openssl/evp.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_mtls_hmac_ctx HMAC_CTX;

#define HMAC cwist_mtls_HMAC
#define HMAC_CTX_new cwist_mtls_HMAC_CTX_new
#define HMAC_CTX_free cwist_mtls_HMAC_CTX_free
#define HMAC_Init_ex cwist_mtls_HMAC_Init_ex
#define HMAC_Update cwist_mtls_HMAC_Update
#define HMAC_Final cwist_mtls_HMAC_Final
unsigned char *cwist_mtls_HMAC(const EVP_MD *md, const void *key, int key_len,
                               const unsigned char *msg, size_t msg_len, unsigned char *out,
                               unsigned int *out_len);
HMAC_CTX *cwist_mtls_HMAC_CTX_new(void);
void cwist_mtls_HMAC_CTX_free(HMAC_CTX *ctx);
int cwist_mtls_HMAC_Init_ex(HMAC_CTX *ctx, const void *key, int key_len, const EVP_MD *md,
                            void *engine);
int cwist_mtls_HMAC_Update(HMAC_CTX *ctx, const unsigned char *data, size_t len);
int cwist_mtls_HMAC_Final(HMAC_CTX *ctx, unsigned char *out, unsigned int *out_len);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_HMAC_H */
