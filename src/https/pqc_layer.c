/**
 * @file pqc_layer.c
 * @brief Post-Quantum Cryptography (PQC) TLS layer facade.
 *
 * Internal-only file. No application code should include this.
 */

#include <cwist/sys/app/app.h>
#include <cwist/core/log.h>
#include <openssl/ssl.h>
#include <string.h>

/**
 * @brief Apply PQC hybrid key exchange settings to an SSL context.
 * @param app Application context containing PQC configuration.
 * @param ctx OpenSSL/BoringSSL SSL_CTX to mutate.
 * @return true when PQC was applied or not needed; false on failure.
 */
bool cwist_tls_apply_pqc_layer(cwist_app *app, SSL_CTX *ctx) {
    if (!ctx) return false;

    if (!app || !app->pqc_layer_enabled) return true;

#ifdef CWIST_TLS_MBEDTLS
    /* mbedTLS 3.6 LTS has no hybrid post-quantum key exchange (X25519MLKEM768
     * is BoringSSL-only): degrade to TLS 1.3 with classical groups instead of
     * silently pretending PQC is on. */
    CWIST_LOG_WARN("PQC layer: mbedTLS backend has no hybrid post-quantum groups; "
                   "enforcing TLS 1.3 with classical key exchange");
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1) {
        CWIST_LOG_ERROR("PQC layer requires TLS 1.3");
        return false;
    }
    return true;
#else
    const char *groups = app->tls_groups;
    if (!groups) {
        groups = "X25519MLKEM768:X25519:P-256";
    }

    if (SSL_CTX_set1_groups_list(ctx, groups) != 1) {
        CWIST_LOG_ERROR("PQC layer rejected hybrid TLS groups: %s", groups);
        return false;
    }

    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1) {
        CWIST_LOG_ERROR("PQC layer requires TLS 1.3");
        return false;
    }

    SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_NO_TLSv1_2);

    CWIST_LOG_INFO("PQC Layer enabled: groups=%s protocol=TLS1.3", groups);

    return true;
#endif
}
