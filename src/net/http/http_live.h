/**
 * @file http_live.h
 * @brief Classic-pool connection registry hooks for shutdown (private).
 *
 * A classic pool worker registers the connection it serves. Handlers bracket
 * the blocking wait for the next request on a keep-alive connection with
 * cwist_http_live_idle_enter()/_leave(), so shutdown can close idle
 * connections at once and let in-flight requests finish. Outside a classic
 * pool worker both calls are no-ops.
 */

#ifndef CWIST_NET_HTTP_HTTP_LIVE_H
#define CWIST_NET_HTTP_HTTP_LIVE_H

#include <stdbool.h>

/**
 * Mark the calling worker's connection idle (waiting for a new request).
 * @return false when the pool is shutting down: the caller must treat the
 *         connection as closed by the peer instead of waiting on it.
 */
bool cwist_http_live_idle_enter(void);

/** The connection received request bytes; it is no longer idle. */
void cwist_http_live_idle_leave(void);

#endif /* CWIST_NET_HTTP_HTTP_LIVE_H */
