/**
 * @file shim/openssl/opensslv.h
 * @brief mbedTLS TLS backend: version marker.
 *
 * Reports an OpenSSL 3.0-level version: feature tests such as
 * `OPENSSL_VERSION_NUMBER >= 0x30200000L` (the ECH gate in
 * src/security/tls/ech.c) evaluate false, which is the honest answer -
 * mbedTLS 3.6 LTS provides no ECH.
 */
#ifndef CWIST_SHIM_OPENSSL_OPENSSLV_H
#define CWIST_SHIM_OPENSSL_OPENSSLV_H

#define OPENSSL_VERSION_NUMBER 0x30000000L

#endif /* CWIST_SHIM_OPENSSL_OPENSSLV_H */
