/**
 * @file shim/openssl/evp.h
 * @brief mbedTLS TLS backend: EVP subset (HMAC digest tags, AES-256-CBC).
 *
 * Part of the CWIST TLS backend seam; see shim/openssl/ssl.h for the design.
 * Only what CWIST call sites use: EVP_sha256() as an HMAC digest selector
 * and the AES-256-CBC cipher context API used by db_crypt and the (dormant
 * under mbedTLS) session-ticket key callback.
 */
#ifndef CWIST_SHIM_OPENSSL_EVP_H
#define CWIST_SHIM_OPENSSL_EVP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_mtls_evp_md EVP_MD;
typedef struct cwist_mtls_cipher EVP_CIPHER;
typedef struct cwist_mtls_cipher_ctx EVP_CIPHER_CTX;

#define EVP_MAX_MD_SIZE 64

#define EVP_sha256 cwist_mtls_EVP_sha256
#define EVP_aes_256_cbc cwist_mtls_EVP_aes_256_cbc
const EVP_MD *cwist_mtls_EVP_sha256(void);
const EVP_CIPHER *cwist_mtls_EVP_aes_256_cbc(void);

/* Block cipher context (AES-256-CBC with PKCS#7 padding, matching EVP
 * defaults so db_crypt ciphertexts stay format-compatible). */
#define EVP_CIPHER_CTX_new cwist_mtls_EVP_CIPHER_CTX_new
#define EVP_CIPHER_CTX_free cwist_mtls_EVP_CIPHER_CTX_free
#define EVP_EncryptInit_ex cwist_mtls_EVP_EncryptInit_ex
#define EVP_EncryptUpdate cwist_mtls_EVP_EncryptUpdate
#define EVP_EncryptFinal_ex cwist_mtls_EVP_EncryptFinal_ex
#define EVP_DecryptInit_ex cwist_mtls_EVP_DecryptInit_ex
#define EVP_DecryptUpdate cwist_mtls_EVP_DecryptUpdate
#define EVP_DecryptFinal_ex cwist_mtls_EVP_DecryptFinal_ex
EVP_CIPHER_CTX *cwist_mtls_EVP_CIPHER_CTX_new(void);
void cwist_mtls_EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);
int cwist_mtls_EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, void *engine,
                                  const unsigned char *key, const unsigned char *iv);
int cwist_mtls_EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                                 const unsigned char *in, int inl);
int cwist_mtls_EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl);
int cwist_mtls_EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, void *engine,
                                  const unsigned char *key, const unsigned char *iv);
int cwist_mtls_EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                                 const unsigned char *in, int inl);
int cwist_mtls_EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_EVP_H */
