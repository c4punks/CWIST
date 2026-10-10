/**
 * @file async_internal.h
 * @brief Private wiring between the deferred-response path (async.c) and the
 *        C1M connection engine (http.c).
 */

#ifndef CWIST_NET_HTTP_ASYNC_INTERNAL_H
#define CWIST_NET_HTTP_ASYNC_INTERNAL_H

#include <cwist/net/http/http.h>
#include <cwist/sys/io/reactor.h>
#include <stdbool.h>
#include <stdint.h>

/** Current generation of the C1M worker reactors; changes when the pool is destroyed. */
uint64_t cwist_http_reactor_generation(void);

/**
 * Post @p node to @p reactor only while the reactor still belongs to
 * generation @p gen. Returns false, without posting, once the pool that owned
 * it has been destroyed.
 */
bool cwist_http_reactor_post_live(cwist_reactor_t *reactor, uint64_t gen,
                                  cwist_reactor_post_t *node);

/**
 * Close a deferred connection whose worker reactor is gone: close @p client_fd
 * and release @p conn without touching the worker load counters.
 */
void cwist_http_async_close_orphan(int client_fd, cwist_http_async_conn_t *conn);

/**
 * Return whether cwist_async_defer() handed an HTTP/1.1 HTTPS connection to
 * its completion on this thread since the last call, and clear the flag
 * (deferring an h2 stream leaves the conn with the h2 loop). The HTTPS pool worker checks this
 * instead of conn->deferred: once the dispatch ack is published the
 * completion owns the conn and may already have closed or resubmitted it.
 */
bool cwist_async_https_take_deferred(void);

#endif
