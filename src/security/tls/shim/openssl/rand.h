/**
 * @file shim/openssl/rand.h
 * @brief mbedTLS TLS backend: RAND_bytes over mbedTLS CTR-DRBG.
 *
 * Part of the CWIST TLS backend seam; see shim/openssl/ssl.h for the design.
 * The DRBG is seeded lazily from the system entropy source and re-seeded
 * after fork(), so CWIST's pre-fork worker model never shares DRBG state
 * between worker processes.
 */
#ifndef CWIST_SHIM_OPENSSL_RAND_H
#define CWIST_SHIM_OPENSSL_RAND_H

#ifdef __cplusplus
extern "C" {
#endif

#define RAND_bytes cwist_mtls_RAND_bytes
int cwist_mtls_RAND_bytes(unsigned char *buf, int num);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_RAND_H */
