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

struct cwist_async;

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

/* Dispatch-thread handoff record (async_tls.c). It has no other dependency,
 * so http.c and the middleware link it on every target, WASM included. */

/** Record that @p a now owns @p res; called by cwist_async_defer(). */
void cwist_async_record_pending(struct cwist_async *a, const cwist_http_response *res);

/** Drop this thread's record if it names @p a (ack, final release). */
void cwist_async_forget_handle(const struct cwist_async *a);

/**
 * Dispatch side of the handoff: return the handle cwist_async_defer() created
 * for @p res on this thread, or NULL if the handler did not defer it, and
 * forget it. Dispatch calls this right after the handler returns and uses the
 * result instead of res->deferred/res->async, which a producer can reach and
 * overwrite before the ack. A mismatch between the two is reported (and
 * aborts under CWIST_ASYNC_STRICT=1).
 */
struct cwist_async *cwist_async_dispatch_take(cwist_http_response *res);

/**
 * Return whether @p res is deferred as seen from this thread: either
 * cwist_async_defer() deferred it here and dispatch has not taken it yet, or
 * res->deferred is set. Middleware uses this so a producer that clears
 * res->deferred early cannot make the dispatch thread touch the response.
 */
bool cwist_async_res_deferred(const cwist_http_response *res);

/**
 * Drop this thread's pending handoff if it names @p res, so a response later
 * allocated at the same address is not mistaken for it. Called when a
 * response is destroyed without dispatch having taken it (a defer outside
 * dispatch, or a path such as HTTP/3 that does not hand off).
 */
void cwist_async_forget_response(const cwist_http_response *res);

/**
 * Classic-pool pipelining carry: bytes a plain HTTP/1.1 connection had
 * already read past a deferred request. The classic handler's read buffer
 * lives on its stack and is gone once the handler returns, so dispatch
 * copies the surplus onto the handle before the ack and the completion
 * resubmits the connection with it (cwist_app_http_handler_resume).
 */
typedef struct cwist_http_carry {
    struct cwist_app *app;
    size_t len;
    char bytes[];
} cwist_http_carry;

/**
 * Copy @p len pipelined bytes onto @p a for the classic re-arm. Call on the
 * dispatch thread before cwist_async_dispatch_ack(). On allocation failure the
 * exchange is downgraded to Connection: close, so the client sees the close
 * and retries instead of waiting on requests that were dropped.
 */
void cwist_async_set_carry(struct cwist_async *a, const char *bytes, size_t len);

/** Classic pool handler that first serves a cwist_http_carry (app.c); takes ownership of @p ctx. */
void cwist_app_http_handler_resume(int client_fd, void *ctx);

/* --- Streaming deferred responses (HTTP/1.1 cleartext and TLS) ------------ */

struct cwist_https_connection;

int cwist_http_write_raw(int client_fd, const void *buf, size_t len);
int cwist_http_write_chunk(int client_fd, const void *buf, size_t len);
void cwist_http_async_finish_stream(int client_fd, cwist_reactor_t *reactor,
                                    cwist_http_async_conn_t *conn, bool keep_alive);
int cwist_https_write_raw(struct cwist_https_connection *conn, const void *buf, size_t len);

/** True when @p a is an HTTP/1.1 exchange whose body can be streamed incrementally. */
bool cwist_async_can_stream(const struct cwist_async *a);
/** True once cwist_async_stream_start() has claimed @p a for streaming. */
bool cwist_async_is_streamed(const struct cwist_async *a);
/** Claim @p a and send the status line and headers of @p res immediately. */
bool cwist_async_stream_start(struct cwist_async *a, cwist_http_response *res, bool chunked);
/** Write one body chunk (chunk-framed when @p chunked is true, raw otherwise). */
bool cwist_async_stream_write(struct cwist_async *a, const void *data, size_t len, bool chunked);
/** Emit the terminating chunk (when @p ok && @p chunked) and finish/release @p a. */
void cwist_async_stream_finish(struct cwist_async *a, bool ok, bool chunked);

/* --- HTTP/3 deferred responses ------------------------------------------
 *
 * lsquic runs every HTTP/3 connection of a server loop on one engine thread
 * and is not thread-safe, so a completing producer never touches the stream.
 * It enqueues the finished exchange on the loop's queue and pokes its wake
 * fd; the engine thread drains the queue and resumes the stream. The stream
 * is named by a refcounted token the engine thread detaches when the stream
 * closes first, so a late completion is discarded instead of reaching freed
 * stream state. Implemented in http3.c. */
struct cwist_h3_async_queue;
struct cwist_h3_pending;

struct cwist_h3_async_queue *cwist_h3_async_queue_acquire(struct cwist_h3_async_queue *q);
void cwist_h3_async_queue_release(struct cwist_h3_async_queue *q);
struct cwist_h3_pending *cwist_h3_pending_acquire(struct cwist_h3_pending *p);
void cwist_h3_pending_release(struct cwist_h3_pending *p);
/** Consumes req/send/res and one reference on @p p, whatever it returns. */
int cwist_h3_async_queue_enqueue(struct cwist_h3_async_queue *q, struct cwist_h3_pending *p,
                                 cwist_http_request *req, cwist_http_response *send,
                                 cwist_http_response *res, bool send_owned);

/**
 * Engine-thread bracket around one HTTP/3 handler call: a cwist_async_defer()
 * of @p req on this thread routes its completion through @p q / @p p.
 * cwist_async_h3_end() clears the target and reports whether it deferred.
 */
void cwist_async_h3_begin(cwist_http_request *req, struct cwist_h3_async_queue *q,
                          struct cwist_h3_pending *p);
bool cwist_async_h3_end(void);

/**
 * @brief Give the HTTPS pool thread serving @p conn to a waiting task.
 *
 * Re-queues the connection at the tail of the pool's task queue when other
 * tasks are waiting, so one busy connection cannot hold a pool thread while
 * other connections queue behind it. The caller must have no in-flight
 * work on the connection and, on true, must return without touching
 * @p conn (another pool thread may already be serving it).
 * @return true when the connection was re-queued; false when nothing is
 *         waiting or @p conn is not a pool-served connection.
 */
struct cwist_https_connection;
bool cwist_https_yield(struct cwist_https_connection *conn);

#endif
