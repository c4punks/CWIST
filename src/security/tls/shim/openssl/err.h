/**
 * @file shim/openssl/err.h
 * @brief mbedTLS TLS backend: error queue subset.
 *
 * Part of the CWIST TLS backend seam; see shim/openssl/ssl.h for the design.
 * The "error queue" is a single thread-local slot holding the last mbedTLS
 * error code; ERR_error_string_n renders it with mbedtls_strerror().
 */
#ifndef CWIST_SHIM_OPENSSL_ERR_H
#define CWIST_SHIM_OPENSSL_ERR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ERR_get_error cwist_mtls_ERR_get_error
#define ERR_error_string_n cwist_mtls_ERR_error_string_n
#define ERR_clear_error cwist_mtls_ERR_clear_error
unsigned long cwist_mtls_ERR_get_error(void);
void cwist_mtls_ERR_error_string_n(unsigned long e, char *buf, size_t len);
void cwist_mtls_ERR_clear_error(void);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_ERR_H */
