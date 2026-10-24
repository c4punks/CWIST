/**
 * @file shim/openssl/crypto.h
 * @brief mbedTLS TLS backend: OPENSSL_cleanse / CRYPTO_memcmp.
 *
 * Part of the CWIST TLS backend seam; see shim/openssl/ssl.h for the design.
 */
#ifndef CWIST_SHIM_OPENSSL_CRYPTO_H
#define CWIST_SHIM_OPENSSL_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENSSL_cleanse cwist_mtls_OPENSSL_cleanse
#define CRYPTO_memcmp cwist_mtls_CRYPTO_memcmp
void cwist_mtls_OPENSSL_cleanse(void *ptr, size_t len);
int cwist_mtls_CRYPTO_memcmp(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_CRYPTO_H */
