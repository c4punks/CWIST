/**
 * @file https.h
 * @brief HTTPS/TLS wrapper interface.
 */

#ifndef __CWIST_HTTPS_H__
#define __CWIST_HTTPS_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define HTTPS_THREAD_POOL_SIZE HTTP_THREAD_POOL_SIZE;

/** --- SSL Structures --- */

typedef enum cwist_https_protocol {
    CWIST_HTTPS_PROTOCOL_NONE = 0,
    CWIST_HTTPS_PROTOCOL_HTTP11,
    CWIST_HTTPS_PROTOCOL_HTTP2,
    CWIST_HTTPS_PROTOCOL_HTTP3
} cwist_https_protocol;

typedef struct cwist_https_context {
    SSL_CTX *ctx;
    bool http2_enabled;
    bool http3_enabled;
    void *ticket_key; ///< Shared session-ticket key material (forked workers inherit it)
} cwist_https_context;

typedef struct cwist_https_connection {
    int fd;
    SSL *ssl;
    char *read_buf;
    size_t buf_len;
    bool negotiated_http2;
    cwist_https_protocol negotiated_protocol;
    bool http3_enabled;
    bool http2_sequenced_data; /*< Enable CWIST-specific sequenced DATA frames. */
    bool deferred;             /*< True when response processing has been deferred. */
    /* Idle parking (cwist_https_park). Internal to the HTTPS pool. */
    bool park_expired;         /*< Resumed because the parked idle deadline passed. */
    void *proto_state;         /*< Protocol session kept while parked (HTTP/2). */
    void (*proto_state_free)(void *state);
    struct cwist_https_context *pool_ctx; /*< Pool task that serves this connection. */
    void (*pool_handler)(struct cwist_https_connection *, void *);
    void *pool_user_ctx;
} cwist_https_connection;

/** Idle class of a parked connection; each class has its own expiry order. */
typedef enum cwist_https_park_class {
    CWIST_HTTPS_PARK_HTTP1 = 0, /*< Expiry closes the connection. */
    CWIST_HTTPS_PARK_HTTP2 = 1  /*< Expiry resubmits it with park_expired set (GOAWAY). */
} cwist_https_park_class;

typedef struct cwist_app cwist_app;

typedef struct cwist_https_options {
    bool enable_http2;
    bool enable_http3;
} cwist_https_options;

/** --- API Functions --- */

/**
 * @brief Hand an idle connection back to the HTTPS pool's park set.
 *
 * A pool thread that has nothing to read on @p conn calls this instead of
 * blocking in poll(): the connection is watched by one epoll thread and
 * resubmitted to the pool when bytes arrive, so an idle keep-alive
 * connection costs memory but no thread. Returns true when the connection
 * was parked; the caller must then return without touching @p conn again
 * (another pool thread may already be serving it). Returns false when
 * parking is unavailable (full GC, CWIST_HTTPS_PARK=0, shutdown); the
 * caller keeps serving the connection the blocking way.
 * @param idle_ms Idle budget before the class-specific expiry action.
 */
bool cwist_https_park(cwist_https_connection *conn, uint64_t idle_ms, cwist_https_park_class cls);

/** @brief True when @p conn has no buffered TLS bytes and nothing readable. */
bool cwist_https_conn_idle(cwist_https_connection *conn);

/**
 * @brief Idle keep-alive budget for parked HTTP/1.1 TLS connections, in ms.
 * CWIST_HTTPS_IDLE_TIMEOUT_MS overrides the default (CWIST_HTTP_TIMEOUT_MS,
 * the same 30 s the blocking header read waited).
 */
uint64_t cwist_https_idle_timeout_ms(void);

/**
 * Initialize the OpenSSL library and create an SSL context.
 * Loads certificate and private key.
 */
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path,
                                       const char *key_path);

/**
 * Initialize an HTTPS context with explicit transport options.
 * The HTTP/2 option only applies the standard TLS/ALPN profile today.
 * Application request handling remains HTTP/1.1 unless a frame engine is added.
 */
cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path, const char *key_path,
                                                    const cwist_https_options *options,
                                                    cwist_app *app);

/**
 * Destroy the HTTPS context and cleanup OpenSSL.
 */
void cwist_https_destroy_context(cwist_https_context *ctx);

/**
 * Perform SSL handshake on an accepted socket.
 * Returns a new cwist_https_connection wrapper.
 */
cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd,
                                 cwist_https_connection **conn);

/**
 * Returns true when ALPN negotiated h2 on this TLS connection.
 */
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn);

/**
 * Returns the protocol selected by ALPN, or HTTP/1.1 when no higher protocol matched.
 */
cwist_https_protocol cwist_https_connection_protocol(const cwist_https_connection *conn);

/**
 * Close and free the HTTPS connection.
 */
void cwist_https_close_connection(cwist_https_connection *conn);

/**
 * Read data from the SSL connection and parse it as an HTTP request.
 * Uses cwist_http_parse_request internally.
 */
cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn);

/**
 * Serialize an HTTP response and send it over the SSL connection.
 * Streams the header block and body separately (zero-copy; no intermediate
 * serialization blob) and supports pointer bodies and file streams.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);
cwist_error_t cwist_https_send_response_head(cwist_https_connection *conn,
                                             cwist_http_response *res);

/**
 * Helper to start a simple HTTPS server loop.
 * Note: The handler receives a cwist_https_connection pointer, not an int fd.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx,
                                      void (*handler)(cwist_https_connection *conn, void *),
                                      void *user_ctx);

/**
 * Thread pool helpers for hybrid async-accept + threaded-process mode.
 */
int https_pool_init(void);
void https_pool_submit(int client_fd, cwist_https_context *ctx,
                       void (*handler)(cwist_https_connection *, void *), void *user_ctx);
void https_pool_submit_conn(cwist_https_connection *conn, cwist_https_context *ctx,
                            void (*handler)(cwist_https_connection *, void *), void *user_ctx);
void https_pool_destroy(void);

/**
 * @brief Accept a TLS connection without parking a worker on the handshake.
 * Completes the handshake inline when the ClientHello is already pending,
 * otherwise parks the connection with the handshake shepherd thread and
 * returns immediately.  Established sessions are submitted to the HTTPS
 * worker pool.  Safe to call from reactor callbacks.
 */
void cwist_https_dispatch(int client_fd, cwist_https_context *ctx,
                          void (*handler)(cwist_https_connection *, void *), void *user_ctx);

/** @brief Number of TLS handshakes currently parked in the shepherd. */
long cwist_https_pending_handshakes(void);

/* --- TLS observability counters (mirrored into the Prometheus /metrics
 * exposition by the metrics registry; see src/sys/metrics/metrics.c) --- */

/** @brief Total TLS handshakes that completed (full + resumed). */
long cwist_https_tls_handshakes_total(void);
/** @brief TLS handshakes completed via session resumption/ticket. */
long cwist_https_tls_handshakes_resumed_total(void);
/** @brief Handshakes that negotiated TLS 1.2. */
long cwist_https_tls_handshakes_tls12_total(void);
/** @brief Handshakes that negotiated TLS 1.3. */
long cwist_https_tls_handshakes_tls13_total(void);
/** @brief Handshakes negotiated with TLS_AES_128_GCM_SHA256. */
long cwist_https_tls_ciphers_aes128_gcm_total(void);
/** @brief Handshakes negotiated with TLS_AES_256_GCM_SHA384. */
long cwist_https_tls_ciphers_aes256_gcm_total(void);
/** @brief Handshakes negotiated with TLS_CHACHA20_POLY1305_SHA256. */
long cwist_https_tls_ciphers_chacha20_total(void);
/** @brief Handshakes negotiated with any other cipher. */
long cwist_https_tls_ciphers_other_total(void);
/** @brief Currently established TLS connections (wrap minus teardown). */
long cwist_https_tls_connections_active(void);

/** --- Error Codes --- */
/**
 * @brief Defined as errno-like constants used with `cwist_error_t` fields.
 * Values are declared elsewhere alongside their implementations.
 */

#endif
