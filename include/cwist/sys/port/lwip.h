/**
 * @file lwip.h
 * @brief Serve a CWIST app over lwIP's raw TCP API (issue #447 Phase 3).
 *
 * For targets without BSD sockets (bare metal, or an RTOS running lwIP in
 * NO_SYS / tcpip-thread mode): the adapter owns a listening tcp_pcb, frames
 * HTTP/1.1 requests out of the received pbufs, runs them through the app's
 * router and middleware (cwist_app_dispatch), and streams the serialized
 * response back with tcp_write/tcp_sent back-pressure.
 *
 * Build: compile src/sys/port/lwip.c into the firmware next to lwIP, with
 * the firmware's lwipopts.h on the include path (the adapter depends on its
 * TCP options). It needs LWIP_TCP and LWIP_CALLBACK_API, and uses only the
 * raw API, so it works under NO_SYS=1. With SO_REUSE=1 the listener sets
 * SOF_REUSEADDR, so it can be re-created while closed connections sit in
 * TIME_WAIT.
 *
 * Context: every call here, and every callback the adapter installs, runs in
 * lwIP's core context: the main loop for NO_SYS=1, the tcpip thread
 * otherwise (wrap calls in tcpip_callback()). Handlers therefore run there
 * too; keep them short.
 *
 * Behaviour:
 *   - Request bodies are framed by Content-Length. Transfer-Encoding is
 *     answered with 501, Content-Length together with Transfer-Encoding or an
 *     invalid/conflicting Content-Length with 400, an oversized header block
 *     with 431 and an oversized body with 413; the connection then closes.
 *   - Expect: 100-continue gets an interim 100 before the body is read.
 *   - Keep-alive follows the request and the response (as on the socket
 *     path); pipelined requests are answered in order, one at a time.
 *   - Responses are sent zero-copy from per-connection serialized storage
 *     allocated once from a static pool at listen time; a response larger
 *     than the buffer is streamed through it in chunks as the peer
 *     acknowledges, with the response object retained until the last chunk.
 *   - Received data beyond the request limits is refused back to lwIP while
 *     a response is in flight, so the TCP window throttles the peer.
 *   - Connections idle longer than idle_timeout_ms, or beyond max_conns at
 *     accept time, are closed (the latter aborted).
 *   - Deferred (cwist_async) and upgraded (WebSocket) responses are not
 *     supported here.
 *
 * TLS (issue #447 Phase 4): build lwip.c with -DCWIST_LWIP_TLS and link
 * mbedTLS 3.6 (TLS 1.2 server, X.509, PK parse, CTR_DRBG), then set
 * cfg.tls to a context from cwist_lwip_tls_create(). The adapter drives
 * mbedTLS itself over the pcb (lwIP's altcp_tls_mbedtls only supports
 * mbedTLS 2.x). The DRBG is seeded from cwist_port_entropy(); a failing
 * hook makes cwist_lwip_tls_create() fail rather than run unseeded.
 * Ciphertext goes out with TCP_WRITE_FLAG_COPY. Each TLS connection costs
 * one mbedtls_ssl_context plus MBEDTLS_SSL_IN_CONTENT_LEN and
 * MBEDTLS_SSL_OUT_CONTENT_LEN record buffers. A failed handshake aborts the
 * connection. Without CWIST_LWIP_TLS, cwist_lwip_tls_create() returns NULL
 * and the adapter serves plaintext only.
 */

#ifndef __CWIST_SYS_PORT_LWIP_H__
#define __CWIST_SYS_PORT_LWIP_H__

#include <cwist/sys/app/app.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Listener settings; start from cwist_lwip_config_init(). */
typedef struct cwist_lwip_config {
    uint16_t port;            /**< TCP port to listen on (default 80). */
    uint16_t max_conns;       /**< Concurrent connections; extra accepts are aborted (default 4). */
    size_t max_header_bytes;  /**< Request line + headers + CRLFCRLF cap; 431 above (default 4096). */
    size_t max_body_bytes;    /**< Content-Length cap; 413 above (default 16384). */
    size_t rx_buffer_bytes;   /**< Per-connection request buffer in the static pool; a request that
                                   does not fit is refused with 431/413 even when the protocol caps
                                   above allow it (default 2048). */
    size_t tx_buffer_bytes;   /**< Per-connection response buffer in the static pool; responses
                                   larger than this are streamed through the buffer in chunks as
                                   the peer acknowledges them (default 2048). */
    uint32_t idle_timeout_ms; /**< Close a connection with no traffic for this long; 0 = never (default 30000). */
    bool keep_alive;          /**< Allow persistent connections (default true). */
    struct cwist_lwip_tls *tls; /**< Serve HTTPS with this context; NULL = plaintext (default). Not owned. */
} cwist_lwip_config;

/** @brief Opaque TLS server context: certificate chain, key, DRBG and mbedTLS config. */
typedef struct cwist_lwip_tls cwist_lwip_tls;

/** @brief Opaque listener handle returned by cwist_lwip_listen(). */
typedef struct cwist_lwip_server cwist_lwip_server;

/**
 * @brief Fill @p cfg with the defaults documented on cwist_lwip_config.
 * @param cfg Configuration to initialize.
 */
void cwist_lwip_config_init(cwist_lwip_config *cfg);

/**
 * @brief Bind a listening tcp_pcb on @p cfg->port (any address) and serve @p app on it.
 * @param app Application to dispatch to; must outlive the server.
 * @param cfg Settings, copied; NULL means the defaults.
 * @return Server handle, or NULL when the pcb cannot be created, bound or
 *         put into the listen state, or on allocation failure.
 */
cwist_lwip_server *cwist_lwip_listen(cwist_app *app, const cwist_lwip_config *cfg);

/**
 * @brief Stop listening and abort every open connection, then free @p srv.
 * @param srv Server returned by cwist_lwip_listen(); NULL is a no-op.
 */
void cwist_lwip_close(cwist_lwip_server *srv);

/**
 * @brief Number of connections the server currently holds open.
 * @param srv Server handle.
 * @return Open connection count (0 for NULL).
 */
size_t cwist_lwip_active_connections(const cwist_lwip_server *srv);

/**
 * @brief Create a TLS server context from a certificate chain and private key.
 *
 * PEM input must be NUL-terminated and @p cert_len / @p key_len must count
 * the NUL (mbedTLS's PEM rule: pass sizeof() of a string literal); DER input
 * is passed as is. The context may be shared by several servers and must
 * outlive every server that uses it.
 *
 * @param cert Certificate chain, leaf first (PEM or DER).
 * @param cert_len Length of @p cert.
 * @param key Private key matching the leaf certificate (PEM or DER, unencrypted).
 * @param key_len Length of @p key.
 * @return Context, or NULL when parsing fails, the key does not match the
 *         certificate, cwist_port_entropy() fails while seeding the DRBG,
 *         memory runs out, or lwip.c was built without CWIST_LWIP_TLS.
 */
cwist_lwip_tls *cwist_lwip_tls_create(const unsigned char *cert, size_t cert_len,
                                      const unsigned char *key, size_t key_len);

/**
 * @brief Free a TLS context. Close every server using it first.
 * @param tls Context from cwist_lwip_tls_create(); NULL is a no-op.
 */
void cwist_lwip_tls_destroy(cwist_lwip_tls *tls);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SYS_PORT_LWIP_H__ */
