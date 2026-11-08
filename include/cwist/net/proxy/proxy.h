/**
 * @file proxy.h
 * @brief Reverse proxy: forward HTTP/1.1, HTTP/2 and HTTP/3 requests to
 *        HTTP/1.1 upstreams.
 *
 * Rules are registered process-wide, then cwist_proxy_listen() builds one
 * front application per origin port and serves them:
 *
 * @code
 * cwist_proxy.h2_enabled = true;
 * cwist_proxy.h3_enabled = true;
 * cwist_proxy.http_auto_promote = true;
 * cwist_proxy_route("http://api.example.com:8080", "/api", "http://127.0.0.1:3000", "/v1");
 * cwist_proxy_port_forward(8081, 4000);
 * return cwist_proxy_listen();
 * @endcode
 *
 * Security defaults (not configurable in this version):
 *  - The upstream comes only from a registered rule, never from the request.
 *  - Requests are parsed and re-serialized; raw bytes are never forwarded.
 *    Header names must be tokens and values must not contain CR, LF or NUL,
 *    whichever protocol they arrived on.
 *  - Hop-by-hop headers, and every header the Connection header names, are
 *    dropped in both directions.
 *  - X-Forwarded-For/-Host/-Proto, X-Real-IP and Forwarded sent by the client
 *    are discarded; the proxy writes its own X-Forwarded-* values.
 *  - Paths with dot segments, encoded slashes or backslashes, malformed
 *    percent escapes or control bytes are rejected with 400.
 *  - Upstream redirects are returned to the client, never followed, and
 *    upstream TLS certificates are verified.
 *
 * Every front protocol (HTTP/1.1, h2, HTTP/3) is forwarded off the serving
 * thread through cwist_async_defer(); the QUIC engine keeps serving other
 * streams while an upstream answers.
 *
 * On HTTP/1.1 fronts (cleartext and TLS), response headers and body chunks
 * stream to the client as they arrive from the upstream (bounded by
 * cwist_proxy_config::max_response_bytes); HTTP/2 and HTTP/3 fronts assemble
 * the response on the worker before handing it to the connection thread.
 * WebSocket upgrades and CONNECT tunnels pass through as raw byte relays on
 * cleartext HTTP/1.1 fronts (and return 501 on TLS/HTTP-2/HTTP-3 fronts).
 */

#ifndef CWIST_NET_PROXY_PROXY_H
#define CWIST_NET_PROXY_PROXY_H

#include <cwist/sys/err/cwist_err.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process-wide proxy settings, read by cwist_proxy_listen().
 *
 * Change fields before cwist_proxy_listen(); later changes are not applied.
 */
typedef struct cwist_proxy_config {
    /** Serve HTTP/2: h2 over TLS (ALPN), h2c prior knowledge in cleartext. */
    bool h2_enabled;
    /** Serve HTTP/3 on the origin port's UDP side (ephemeral cert without TLS). */
    bool h3_enabled;
    /**
     * Let HTTP/1.1 (and HTTP/2) clients move to the newest enabled protocol:
     * responses advertise `Alt-Svc: h3=":<port>"` when h3 is enabled, and TLS
     * listeners offer h2 through ALPN when h2 is enabled. When false, proxied
     * responses carry `Alt-Svc: clear`, so only clients that already target
     * HTTP/3 use it. Cleartext h2c Upgrade is not offered (RFC 9113 deprecates it).
     */
    bool http_auto_promote;
    /** PEM certificate chain; required by https:// origins, forbidden with http:// ones. */
    const char *cert_path;
    /** PEM private key matching cert_path. */
    const char *key_path;
    /** Send the client's Host upstream instead of the target authority. */
    bool preserve_host;
    /** Upstream connect timeout in milliseconds (default 5000). */
    int connect_timeout_ms;
    /** Whole upstream exchange timeout in milliseconds (default 30000). */
    int timeout_ms;
    /** Largest upstream response body accepted, in bytes (default 64 MiB). */
    size_t max_response_bytes;
    /** Upstream worker threads per process (default 8). */
    size_t workers;
    /**
     * Promote TLS to hybrid post-quantum key exchange (X25519MLKEM768, TLS 1.3)
     * on both sides: front listeners (TCP and QUIC) prefer it, and every
     * https:// upstream must negotiate it or the request fails with 502.
     * When false, an https:// upstream keeps the client's level instead: the
     * same minimum TLS version, and post-quantum key exchange only when the
     * client negotiated it. http:// targets always terminate TLS here.
     */
    bool tls_auto_promote;
    /** PEM CA bundle for verifying https:// upstreams; NULL uses the system store. */
    const char *upstream_ca_path;
} cwist_proxy_config;

/** @brief Settings applied by cwist_proxy_listen(). */
extern cwist_proxy_config cwist_proxy;

/**
 * @brief Forward requests for an origin route to a target route.
 *
 * @param origin       The origin as the browser address bar shows it:
 *                     "http://api.example.com:8080" or "https://api.example.com"
 *                     (port defaults to 80/443 by scheme). Scheme and host are
 *                     required. The proxy listens on the port on every interface
 *                     and serves a request only when its Host names this origin
 *                     exactly (host case-insensitive, port explicit or the scheme
 *                     default), so "http://localhost:8080" does not answer for
 *                     "127.0.0.1:8080". Other Hosts get 421 Misdirected Request.
 * @param origin_route Path prefix on the origin, e.g. "/api". It matches "/api"
 *                     and "/api/..." but not "/apix". "/" matches every path.
 * @param target       Upstream as "http://host:port", "https://host:port" or
 *                     "host:port" (http). No path, query or userinfo.
 * @param target_route Path prefix that replaces @p origin_route upstream, e.g.
 *                     "/v1" sends "/api/users?x=1" to "/v1/users?x=1".
 * @return err_i16 0 on success; -1 on invalid arguments, a duplicate rule, an
 *         http:// and https:// origin on the same port, allocation failure, or
 *         after cwist_proxy_listen() has started.
 *
 * The longest matching origin_route wins; an origin that matches but has no
 * matching route answers 404. All origins currently share one scheme: the
 * listeners are either all TLS or all cleartext.
 */
cwist_error_t cwist_proxy_route(const char *origin, const char *origin_route, const char *target,
                                const char *target_route);

/**
 * @brief Forward every Host and path on @p origin_port to 127.0.0.1:@p target_port.
 *
 * The one rule that ignores the Host header: use it for a plain port-to-port
 * hop. A cwist_proxy_route() origin on the same port takes precedence over it
 * at equal prefix length. The port is TLS when the named origins are https://
 * or, with no named origins, when a certificate is configured.
 */
cwist_error_t cwist_proxy_port_forward(unsigned short origin_port, unsigned short target_port);

/**
 * @brief Serve every registered origin port until shutdown; blocks.
 *
 * One port is served by cwist_app_listen() (reactor or classic pool, per
 * CWIST_C1M_MODE); several ports share cwist_app_multiport().
 * @return 0 after a graceful shutdown, -1 when nothing is registered or the
 *         listeners could not start.
 */
int cwist_proxy_listen(void);

/**
 * @brief Drop every rule and restore the default cwist_proxy settings.
 *
 * Must not run while cwist_proxy_listen() is serving.
 */
void cwist_proxy_reset(void);

#ifdef __cplusplus
}
#endif

#endif
