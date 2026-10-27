/* mbedTLS 3.6 configuration for the lwIP TLS tests (tests/test_lwip_tls.c on
 * the host, tests/baremetal/hello_tls.c on Cortex-M). A small-MCU server
 * profile: TLS 1.2, ECDHE-ECDSA on P-256, AES-128-GCM, SHA-256. The client
 * side is enabled only because the tests drive a TLS client too.
 *
 * Randomness: the adapter seeds CTR_DRBG from cwist_port_entropy() itself,
 * so MBEDTLS_ENTROPY_C and platform entropy stay off. No filesystem, no
 * sockets, no clock (certificate validity dates are not checked). */
#ifndef CWIST_TEST_MBEDTLS_CONFIG_H
#define CWIST_TEST_MBEDTLS_CONFIG_H

/* system */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
/* Clients may send full 16 KB records (no max_fragment_length in browsers);
 * the server's own records can be small. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* X.509 / keys */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* crypto */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C

#define MBEDTLS_ERROR_C

#endif /* CWIST_TEST_MBEDTLS_CONFIG_H */
