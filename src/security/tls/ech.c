/** @file ech.c
 * @brief ech.c interface.
 */
#define _GNU_SOURCE
#include <cwist/security/tls/ech.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include <stdio.h>

/** @brief Enable ECH (Encrypted Client Hello) support for an application.
 *
 * When built against OpenSSL 3.2 or newer with ECH support
 * (SSL_OP_ENABLE_ECH defined), this sets the ECH option on the
 * application's SSL context. On older OpenSSL versions this is a no-op
 * that still succeeds. The ech_key and ech_dir arguments are currently
 * accepted but not used.
 *
 * @param app     Application whose SSL context is configured. Must not be
 *                NULL.
 * @param ech_key Unused placeholder for a future ECH key configuration.
 * @param ech_dir Unused placeholder for a future ECH directory source.
 * @retval CWIST_ERROR_INVALID_PARAM @p app is NULL.
 * @retval 0 Success; ECH was enabled or is unsupported on this build.
 */
cwist_error_t cwist_app_use_ech(cwist_app *app, const char *ech_key, const char *ech_dir) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

#if defined(SSL_OP_ENABLE_ECH) && OPENSSL_VERSION_NUMBER >= 0x30200000L
    if (app->ssl_ctx && app->ssl_ctx->ctx) {
        SSL_CTX_set_options(app->ssl_ctx->ctx, SSL_OP_ENABLE_ECH);
    }
    (void)ech_key;
    (void)ech_dir;
#else
    (void)app;
    (void)ech_key;
    (void)ech_dir;
#endif

    err.error.err_i16 = 0;
    return err;
}
