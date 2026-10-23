/**
 * @file shim/openssl/ssl.h
 * @brief mbedTLS TLS backend: OpenSSL-shaped seam over the CWIST TLS subset.
 *
 * This header (and its siblings in this directory) is the CWIST TLS backend
 * seam.  When the library is built with CWIST_TLS=mbedtls the Makefile puts
 * this directory on the include path instead of lib/boringssl/include, so
 * every call site - public headers, https.c, http2.c, grpc_client.c - keeps
 * its <openssl/ssl.h> include and its SSL_* call shape, but compiles against
 * the declarations below.  Each OpenSSL-flavoured name is a macro alias for a
 * cwist_mtls_* function genuinely implemented on mbedTLS (mbedtls_ssl_*) in
 * src/security/tls/backend_mbedtls.c, so the produced archive carries no
 * BoringSSL object or symbol.
 *
 * Design notes:
 *  - SSL / SSL_CTX / SSL_METHOD are opaque handles; no public struct changes,
 *    so the API baseline is untouched.
 *  - SSL_new() takes a reference on the SSL_CTX exactly like BoringSSL: the
 *    cwist_https_reload_context() hot-swap contract (old SSL_CTX stays alive
 *    until its last connection drains) is preserved by an internal refcount.
 *  - Error codes mirror BoringSSL's SSL_ERROR_* values; SSL_get_error()
 *    reports the outcome of the last operation on that connection.
 *  - Only the subset CWIST actually uses is provided.  Anything outside it
 *    (X509 inspection, PKCS#7, ASN.1, DTLS) stays BoringSSL-only; the modules
 *    that need it (AIA chain autoload, ECH, WebRTC, HTTP/3) are compiled out
 *    or degraded when CWIST_TLS=mbedtls, as documented in the Makefile and
 *    docs/performance/tls-backends.md.
 */
#ifndef CWIST_SHIM_OPENSSL_SSL_H
#define CWIST_SHIM_OPENSSL_SSL_H

#include <stddef.h>
#include <stdint.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque backend objects.  Definitions live in backend_mbedtls.c. */
typedef struct cwist_mtls_ctx SSL_CTX;
typedef struct cwist_mtls_ssl SSL;
typedef struct cwist_mtls_method SSL_METHOD;

/* Protocol version codes (wire values, as in OpenSSL). */
#define TLS1_2_VERSION 0x0303
#define TLS1_3_VERSION 0x0304

/* SSL_get_error() result codes (values match OpenSSL). */
#define SSL_ERROR_NONE 0
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6

/* SSL_CTX_set_options() flags.  Compression and renegotiation are always off
 * in the mbedTLS backend, so the corresponding flags are accepted as no-ops;
 * SSL_OP_NO_TICKET is honoured (session tickets are disabled). */
#define SSL_OP_NO_COMPRESSION 0x0001UL
#define SSL_OP_NO_RENEGOTIATION 0x0002UL
#define SSL_OP_CIPHER_SERVER_PREFERENCE 0x0004UL
#define SSL_OP_NO_TICKET 0x0008UL
#define SSL_OP_NO_TLSv1 0x0010UL
#define SSL_OP_NO_TLSv1_1 0x0020UL
#define SSL_OP_NO_TLSv1_2 0x0040UL

/* SSL_CTX_set_mode() flags; AUTO_RETRY is the mbedTLS default behaviour. */
#define SSL_MODE_AUTO_RETRY 0x0001UL

/* SSL_CTX_set_session_cache_mode() flags. */
#define SSL_SESS_CACHE_SERVER 0x0002L

/* Certificate file types. */
#define SSL_FILETYPE_PEM 1

/* SSL_CTX_set_verify() modes. */
#define SSL_VERIFY_NONE 0x00
#define SSL_VERIFY_PEER 0x01

/* ALPN select-callback return codes (values match OpenSSL). */
#define SSL_TLSEXT_ERR_OK 0
#define SSL_TLSEXT_ERR_NOACK 3

/* SSL_select_next_proto() return code (value matches OpenSSL). */
#define OPENSSL_NPN_NEGOTIATED 1

/* --- Library lifecycle (no-ops; mbedTLS needs no global init) --- */
#define SSL_load_error_strings() ((void)0)
#define OpenSSL_add_ssl_algorithms() ((void)0)
#define EVP_cleanup() ((void)0)

/* --- SSL_METHOD / SSL_CTX lifecycle --- */
#define TLS_server_method cwist_mtls_TLS_server_method
#define TLS_client_method cwist_mtls_TLS_client_method
#define SSL_CTX_new cwist_mtls_SSL_CTX_new
#define SSL_CTX_free cwist_mtls_SSL_CTX_free
const SSL_METHOD *cwist_mtls_TLS_server_method(void);
const SSL_METHOD *cwist_mtls_TLS_client_method(void);
SSL_CTX *cwist_mtls_SSL_CTX_new(const SSL_METHOD *method);
void cwist_mtls_SSL_CTX_free(SSL_CTX *ctx);

/* --- SSL_CTX configuration --- */
#define SSL_CTX_set_min_proto_version cwist_mtls_SSL_CTX_set_min_proto_version
#define SSL_CTX_set_options cwist_mtls_SSL_CTX_set_options
#define SSL_CTX_set_mode cwist_mtls_SSL_CTX_set_mode
#define SSL_CTX_set_cipher_list cwist_mtls_SSL_CTX_set_cipher_list
#define SSL_CTX_set_ciphersuites cwist_mtls_SSL_CTX_set_ciphersuites
#define SSL_CTX_use_certificate_chain_file cwist_mtls_SSL_CTX_use_certificate_chain_file
#define SSL_CTX_use_PrivateKey_file cwist_mtls_SSL_CTX_use_PrivateKey_file
#define SSL_CTX_check_private_key cwist_mtls_SSL_CTX_check_private_key
#define SSL_CTX_set_session_cache_mode cwist_mtls_SSL_CTX_set_session_cache_mode
#define SSL_CTX_sess_set_cache_size cwist_mtls_SSL_CTX_sess_set_cache_size
#define SSL_CTX_set_session_id_context cwist_mtls_SSL_CTX_set_session_id_context
#define SSL_CTX_set_timeout cwist_mtls_SSL_CTX_set_timeout
#define SSL_CTX_set_alpn_select_cb cwist_mtls_SSL_CTX_set_alpn_select_cb
#define SSL_CTX_set_alpn_protos cwist_mtls_SSL_CTX_set_alpn_protos
#define SSL_CTX_set_tlsext_ticket_key_cb cwist_mtls_SSL_CTX_set_tlsext_ticket_key_cb
#define SSL_CTX_get_ex_new_index cwist_mtls_SSL_CTX_get_ex_new_index
#define SSL_CTX_set_ex_data cwist_mtls_SSL_CTX_set_ex_data
#define SSL_CTX_get_ex_data cwist_mtls_SSL_CTX_get_ex_data
#define SSL_CTX_set_verify cwist_mtls_SSL_CTX_set_verify
#define SSL_CTX_set_default_verify_paths cwist_mtls_SSL_CTX_set_default_verify_paths
#define SSL_CTX_set1_groups_list cwist_mtls_SSL_CTX_set1_groups_list
int cwist_mtls_SSL_CTX_set_min_proto_version(SSL_CTX *ctx, uint16_t version);
long cwist_mtls_SSL_CTX_set_options(SSL_CTX *ctx, unsigned long options);
long cwist_mtls_SSL_CTX_set_mode(SSL_CTX *ctx, long mode);
int cwist_mtls_SSL_CTX_set_cipher_list(SSL_CTX *ctx, const char *list);
int cwist_mtls_SSL_CTX_set_ciphersuites(SSL_CTX *ctx, const char *list);
int cwist_mtls_SSL_CTX_use_certificate_chain_file(SSL_CTX *ctx, const char *path);
int cwist_mtls_SSL_CTX_use_PrivateKey_file(SSL_CTX *ctx, const char *path, int type);
int cwist_mtls_SSL_CTX_check_private_key(const SSL_CTX *ctx);
long cwist_mtls_SSL_CTX_set_session_cache_mode(SSL_CTX *ctx, long mode);
long cwist_mtls_SSL_CTX_sess_set_cache_size(SSL_CTX *ctx, unsigned long size);
int cwist_mtls_SSL_CTX_set_session_id_context(SSL_CTX *ctx, const unsigned char *sid_ctx,
                                              unsigned int sid_ctx_len);
long cwist_mtls_SSL_CTX_set_timeout(SSL_CTX *ctx, long seconds);
void cwist_mtls_SSL_CTX_set_alpn_select_cb(
    SSL_CTX *ctx,
    int (*cb)(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
              unsigned int inlen, void *arg),
    void *arg);
int cwist_mtls_SSL_CTX_set_alpn_protos(SSL_CTX *ctx, const unsigned char *protos,
                                       unsigned int protos_len);
#define SSL_set_alpn_protos cwist_mtls_SSL_set_alpn_protos
int cwist_mtls_SSL_set_alpn_protos(SSL *ssl, const unsigned char *protos, unsigned int protos_len);
#define SSL_CTX_use_certificate_file cwist_mtls_SSL_CTX_use_certificate_file
int cwist_mtls_SSL_CTX_use_certificate_file(SSL_CTX *ctx, const char *path, int type);
#define SSL_CTX_get_min_proto_version cwist_mtls_SSL_CTX_get_min_proto_version
int cwist_mtls_SSL_CTX_get_min_proto_version(const SSL_CTX *ctx);
void cwist_mtls_SSL_CTX_set_tlsext_ticket_key_cb(
    SSL_CTX *ctx,
    int (*cb)(SSL *ssl, unsigned char *key_name, unsigned char *iv, EVP_CIPHER_CTX *ectx,
              HMAC_CTX *hctx, int enc));
int cwist_mtls_SSL_CTX_get_ex_new_index(long argl, void *argp, void *new_func, void *dup_func,
                                        void *free_func);
int cwist_mtls_SSL_CTX_set_ex_data(SSL_CTX *ctx, int idx, void *data);
void *cwist_mtls_SSL_CTX_get_ex_data(const SSL_CTX *ctx, int idx);
void cwist_mtls_SSL_CTX_set_verify(SSL_CTX *ctx, int mode, void *callback);
int cwist_mtls_SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);
int cwist_mtls_SSL_CTX_set1_groups_list(SSL_CTX *ctx, const char *groups);

/* --- Per-connection API --- */
#define SSL_new cwist_mtls_SSL_new
#define SSL_free cwist_mtls_SSL_free
#define SSL_set_fd cwist_mtls_SSL_set_fd
#define SSL_accept cwist_mtls_SSL_accept
#define SSL_connect cwist_mtls_SSL_connect
#define SSL_read cwist_mtls_SSL_read
#define SSL_write cwist_mtls_SSL_write
#define SSL_pending cwist_mtls_SSL_pending
#define SSL_shutdown cwist_mtls_SSL_shutdown
#define SSL_get_error cwist_mtls_SSL_get_error
#define SSL_get_SSL_CTX cwist_mtls_SSL_get_SSL_CTX
#define SSL_session_reused cwist_mtls_SSL_session_reused
#define SSL_version cwist_mtls_SSL_version
#define SSL_get_cipher_name cwist_mtls_SSL_get_cipher_name
#define SSL_get0_alpn_selected cwist_mtls_SSL_get0_alpn_selected
#define SSL_set_tlsext_host_name cwist_mtls_SSL_set_tlsext_host_name
SSL *cwist_mtls_SSL_new(SSL_CTX *ctx);
void cwist_mtls_SSL_free(SSL *ssl);
int cwist_mtls_SSL_set_fd(SSL *ssl, int fd);
int cwist_mtls_SSL_accept(SSL *ssl);
int cwist_mtls_SSL_connect(SSL *ssl);
int cwist_mtls_SSL_read(SSL *ssl, void *buf, int num);
int cwist_mtls_SSL_write(SSL *ssl, const void *buf, int num);
int cwist_mtls_SSL_pending(const SSL *ssl);
int cwist_mtls_SSL_shutdown(SSL *ssl);
int cwist_mtls_SSL_get_error(const SSL *ssl, int ret);
SSL_CTX *cwist_mtls_SSL_get_SSL_CTX(const SSL *ssl);
int cwist_mtls_SSL_session_reused(const SSL *ssl);
int cwist_mtls_SSL_version(const SSL *ssl);
const char *cwist_mtls_SSL_get_cipher_name(const SSL *ssl);
void cwist_mtls_SSL_get0_alpn_selected(const SSL *ssl, const unsigned char **data,
                                       unsigned int *len);
int cwist_mtls_SSL_set_tlsext_host_name(SSL *ssl, const char *name);

/* --- ALPN helpers --- */
#define SSL_select_next_proto cwist_mtls_SSL_select_next_proto
int cwist_mtls_SSL_select_next_proto(unsigned char **out, unsigned char *outlen,
                                     const unsigned char *server, unsigned int server_len,
                                     const unsigned char *client, unsigned int client_len);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SHIM_OPENSSL_SSL_H */
