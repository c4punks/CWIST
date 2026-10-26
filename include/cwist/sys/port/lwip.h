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
 *   - Responses are sent zero-copy from the serialized buffer, which is
 *     released once the peer has acknowledged it.
 *   - Received data beyond the request limits is refused back to lwIP while
 *     a response is in flight, so the TCP window throttles the peer.
 *   - Connections idle longer than idle_timeout_ms, or beyond max_conns at
 *     accept time, are closed (the latter aborted).
 *   - Deferred (cwist_async) and upgraded (WebSocket) responses are not
 *     supported here.
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
    uint32_t idle_timeout_ms; /**< Close a connection with no traffic for this long; 0 = never (default 30000). */
    bool keep_alive;          /**< Allow persistent connections (default true). */
} cwist_lwip_config;

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

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SYS_PORT_LWIP_H__ */
