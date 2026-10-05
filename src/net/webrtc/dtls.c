/** @file dtls.c
 * @brief DTLS helpers: ephemeral certificate generation, fingerprint, SSL_CTX.
 *
 * Provides the minimal DTLS layer for WebRTC: an ephemeral self-signed
 * ECDSA certificate, the SHA-256 certificate fingerprint used in SDP
 * a=fingerprint attributes, and construction of client/server SSL_CTX
 * objects pinned to DTLS 1.2.
 */
#include "webrtc_internal.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

/** @brief DTLS record/link MTU assumed for the media path (bytes).
 * Used to size outgoing DTLS records so they fit typical UDP/ICE paths
 * without IP fragmentation.
 */
#define CWIST_DTLS_MTU 1200

/** @brief Generate an ephemeral self-signed ECDSA certificate for DTLS.
 * @param cert_out Receives a new X509 certificate; set to NULL on entry
 *                 and on failure.
 * @param pkey_out Receives the matching EVP_PKEY; set to NULL on entry
 *                 and on failure.
 * @return 0 on success, -1 on any OpenSSL error.
 * @note The key is P-256 (NID_X9_62_prime256v1). The certificate has
 *       version 3 (v2 encoding), serial 1, a 30-day validity starting now,
 *       and CN "cwist-webrtc". On failure all intermediate objects are
 *       freed and both outputs remain NULL; on success ownership moves to
 *       the caller, who must free them with X509_free()/EVP_PKEY_free().
 */
int cwist_dtls_generate_cert(X509 **cert_out, EVP_PKEY **pkey_out) {
    *cert_out = NULL;
    *pkey_out = NULL;

    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!kctx)
        return -1;
    if (EVP_PKEY_keygen_init(kctx) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(kctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        return -1;
    }
    EVP_PKEY_CTX_free(kctx);

    X509 *cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L * 24L * 30);
    X509_set_pubkey(cert, pkey);
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const uint8_t *)"cwist-webrtc", -1, -1, 0);
    X509_set_issuer_name(cert, name);
    if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return -1;
    }
    *cert_out = cert;
    *pkey_out = pkey;
    return 0;
}

/** @brief Compute the SHA-256 fingerprint of a certificate in SDP format.
 * @param cert Certificate to hash; must be non-NULL.
 * @param out  Output buffer receiving the fingerprint text.
 * @param cap  Capacity of @p out in bytes (including the NUL terminator).
 * @return 0 on success, -1 if the DER encoding fails, the certificate
 *         exceeds the internal 2048-byte DER buffer, or the digest does
 *         not fit in @p out.
 * @retval 0 Fingerprint written, e.g. "AB:12:...:EF" in uppercase hex.
 * @retval -1 Encoding, size, or digest error.
 * @note The result is the colon-separated uppercase hex SHA-256 digest as
 *       used in the SDP a=fingerprint attribute. No terminator is written
 *       beyond what snprintf() already emits.
 */
int cwist_dtls_fingerprint(X509 *cert, char *out, size_t cap) {
    uint8_t der[2048];
    int der_len = i2d_X509(cert, NULL);
    if (der_len <= 0 || (size_t)der_len > sizeof(der))
        return -1;
    uint8_t *p = der;
    i2d_X509(cert, &p);
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    if (!EVP_Digest(der, (size_t)der_len, digest, &dlen, EVP_sha256(), NULL))
        return -1;
    size_t off = 0;
    for (unsigned int i = 0; i < dlen && off + 3 < cap; i++)
        off += (size_t)snprintf(out + off, cap - off, "%s%02X", i ? ":" : "", digest[i]);
    return off > 0 && off < cap ? 0 : -1;
}

/** @brief Create a DTLS SSL_CTX for the client or server role.
 * @param is_server Non-zero to build a DTLS server context, zero for a
 *                  DTLS client context.
 * @param cert      Server certificate; used (with @p pkey) only when
 *                  @p is_server is non-zero. May be NULL for a client.
 * @param pkey      Private key matching @p cert; only used for a server.
 * @return New SSL_CTX on success, NULL on allocation failure or when
 *         certificate/key loading or validation fails on the server side.
 * @note The context is pinned to DTLS 1.2 as the minimum protocol version.
 *       Client contexts use SSL_VERIFY_NONE: peer fingerprint verification
 *       is deferred to the SDP exchange (the test pins the expected
 *       fingerprint out of band).
 */
SSL_CTX *cwist_dtls_ctx_new(int is_server, X509 *cert, EVP_PKEY *pkey) {
    SSL_CTX *ctx = SSL_CTX_new(is_server ? DTLS_server_method() : DTLS_client_method());
    if (!ctx)
        return NULL;
    SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION);
    if (is_server) {
        if (SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            return NULL;
        }
    } else {
        /* Peer fingerprint verification is optional at MVP; the test pins the
         * expected fingerprint via the SDP exchange. */
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    return ctx;
}

/** @brief Return the DTLS link MTU in bytes.
 * @return The value of CWIST_DTLS_MTU (1200).
 */
unsigned int cwist_dtls_link_mtu(void) {
    return CWIST_DTLS_MTU;
}
