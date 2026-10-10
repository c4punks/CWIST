/**
 * @file async.c
 * @brief Deferred-response (async handler) implementation.
 *
 * See include/cwist/net/http/async.h for the model.  The dispatch path
 * (app_serve_parsed_request) detects res->deferred right after the handler
 * returns, acknowledges the handoff, and leaves req/res/fd/conn untouched;
 * from then on the cwist_async completion path owns them.
 *
 * Lifetime race: a foreign thread may complete (and thus want to free
 * req/res/the handle) before the dispatch thread has observed res->deferred.
 * A winning producer therefore waits for a->ack before touching the graph.
 * Dispatch first detaches middleware posthandler allocations from its TLS GC,
 * then release-publishes ack. Producer allocations are detached before post.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/async.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/http2.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/job/scheduler.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include "async_gc.h"
#include "async_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum cwist_async_state { CWIST_ASYNC_ST_PENDING = 0, CWIST_ASYNC_ST_CLAIMED };

struct cwist_async {
    _Atomic size_t refs;          /* Completion plus retained producers/timers. */
    _Atomic int state;
    _Atomic bool ack;             /* Dispatch path observed the handoff. */
    pthread_t dispatch_thread;
    /* Serializes a producer's mutation of the exchange with the dispatch
     * ack: before the ack, dispatch still reads req/res (GC disown,
     * keep-alive), so a producer that completes first must not touch them
     * concurrently, and the "who sends" decision below must be atomic.
     * Held only for short, non-blocking sections; nobody waits for the
     * other side while holding it. */
    pthread_mutex_t mu;
    bool finish_on_ack; /* A completion arrived before the ack; ack sends it. Guarded by mu. */
    cwist_http_request *req;
    cwist_http_response *res;     /* Handler's response (request arena). */
    cwist_http_response *final_res;
    bool final_res_owned;         /* final_res came from respond_with. */
    int client_fd;
    bool keep_alive;
    struct cwist_app *app;
    cwist_reactor_t *reactor;     /* NULL on the classic pool path. */
    uint64_t reactor_gen;         /* Pool generation @c reactor belongs to. */
    bool orphaned;                /* @c reactor was destroyed before completion. */
    cwist_http_async_conn_t *conn;
    void *https_conn;             /* cwist_https_connection * on TLS path. */
    cwist_h2_async_queue *h2_queue; /* Per-connection queue on the H2 path. */
    uint32_t h2_stream_id;
    cwist_reactor_post_t post;
};

static void cwist_async_reactor_complete(void *ctx);
static void cwist_async_deliver(cwist_async *a);
static bool cwist_async_publish_locked(cwist_async *a);

static const char *cwist_async_reason(cwist_http_status_t status) {
    switch (status) {
        case CWIST_HTTP_OK: return "OK";
        case CWIST_HTTP_CREATED: return "Created";
        case CWIST_HTTP_NO_CONTENT: return "No Content";
        case CWIST_HTTP_PARTIAL_CONTENT: return "Partial Content";
        case CWIST_HTTP_NOT_MODIFIED: return "Not Modified";
        case CWIST_HTTP_BAD_REQUEST: return "Bad Request";
        case CWIST_HTTP_UNAUTHORIZED: return "Unauthorized";
        case CWIST_HTTP_FORBIDDEN: return "Forbidden";
        case CWIST_HTTP_NOT_FOUND: return "Not Found";
        case CWIST_HTTP_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
        case CWIST_HTTP_INTERNAL_ERROR: return "Internal Server Error";
        case CWIST_HTTP_NOT_IMPLEMENTED: return "Not Implemented";
        case CWIST_HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case CWIST_HTTP_GATEWAY_TIMEOUT: return "Gateway Timeout";
        default: return "Status";
    }
}

static __thread bool t_https_deferred = false;

bool cwist_async_https_take_deferred(void) {
    bool deferred = t_https_deferred;
    t_https_deferred = false;
    return deferred;
}

cwist_async *cwist_async_defer(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || res->deferred) return NULL;
    cwist_async *a = cwist_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    atomic_init(&a->refs, 1);
    atomic_init(&a->state, CWIST_ASYNC_ST_PENDING);
    atomic_init(&a->ack, false);
    if (pthread_mutex_init(&a->mu, NULL) != 0) {
        cwist_free(a);
        return NULL;
    }
    a->dispatch_thread = pthread_self();
    a->req = req;
    a->res = res;
    a->final_res = res;
    a->client_fd = req->client_fd;
    a->keep_alive = req->keep_alive && res->keep_alive;
    a->app = req->app;
    cwist_http_async_conn_t *conn = (cwist_http_async_conn_t *)req->async_conn;
    if (conn) {
        a->reactor = conn->reactor;
        a->reactor_gen = cwist_http_reactor_generation();
        a->conn = conn;
    }
    if (req->https_conn) {
        a->https_conn = req->https_conn;
        ((cwist_https_connection *)req->https_conn)->deferred = true;
        /* Only an HTTP/1.1 completion takes the conn itself; an h2 stream's
         * completion goes through h2_queue and the h2 loop keeps the conn. */
        if (!req->h2_queue) t_https_deferred = true;
    }
    if (req->h2_queue) {
        /* Keep the queue alive until the completion has been enqueued, even
         * if the connection tears down first (teardown closes the queue and
         * releases only its own reference). */
        a->h2_queue = cwist_h2_async_queue_acquire((cwist_h2_async_queue *)req->h2_queue);
        a->h2_stream_id = req->stream_id;
    }
    a->post.cb = cwist_async_reactor_complete;
    a->post.ctx = a;
    /* Explicit async refs, not the creator's TLS GC sweep, own this handle. */
    if (cwist_full_gc_enabled()) cwist_gc_scope_disown(a);
    cwist_http_async_disown_request(req);
    cwist_http_async_disown_response(res);
    res->async = a;
    res->deferred = true;
    return a;
}

cwist_async *cwist_async_retain(cwist_async *a) {
    if (a) atomic_fetch_add_explicit(&a->refs, 1, memory_order_relaxed);
    return a;
}

void cwist_async_release(cwist_async *a) {
    if (a && atomic_fetch_sub_explicit(&a->refs, 1, memory_order_acq_rel) == 1) {
        pthread_mutex_destroy(&a->mu);
        cwist_free(a);
    }
}

void cwist_async_dispatch_ack(cwist_async *a) {
    if (!a) return;
    /* Middleware may allocate after next() returns. No producer may mutate
     * or destroy this graph until the acknowledgement; a producer that
     * already completed waits on mu and left its result to send here. */
    pthread_mutex_lock(&a->mu);
    cwist_http_async_disown_request(a->req);
    cwist_http_async_disown_response(a->res);
    a->keep_alive = a->keep_alive && a->req->keep_alive && a->res->keep_alive;
    bool finish = a->finish_on_ack;
    atomic_store_explicit(&a->ack, true, memory_order_release);
    pthread_mutex_unlock(&a->mu);
    /* Without a recorded completion a foreign producer may free a from
     * here on; with one, this thread owns sending it. */
    if (finish) cwist_async_deliver(a);
}

static bool cwist_async_claim(cwist_async *a) {
    int expected = CWIST_ASYNC_ST_PENDING;
    return atomic_compare_exchange_strong_explicit(&a->state, &expected, CWIST_ASYNC_ST_CLAIMED,
                                                   memory_order_acq_rel, memory_order_acquire);
}

static void cwist_async_reactor_complete(void *ctx);

/* Send the final response, re-arm or close the connection, then release the
 * request/response pair and the handle itself. */
static void cwist_async_complete(cwist_async *a) {
    cwist_http_response *res = a->final_res;
    bool keep = a->keep_alive && atomic_load(&g_cwist_running);
    res->keep_alive = keep;

    if (a->h2_queue) {
        /* Plan B handoff: never write frames from a worker thread.  Enqueue
         * the finished exchange and poke the connection's wake fd; the
         * connection thread drains the queue, HPACK-encodes, and sends.
         * Wait for the dispatch ack first: enqueueing transfers req/res
         * ownership, which is only legal once dispatch observed the defer. */
        while (!atomic_load_explicit(&a->ack, memory_order_acquire)) sched_yield();
        cwist_h2_async_queue_enqueue(a->h2_queue, a->h2_stream_id, a->req, a->final_res, a->res,
                                     a->final_res_owned);
        cwist_h2_async_queue_release(a->h2_queue);
        cwist_async_release(a);
        return;
    }

    if (a->https_conn) {
        cwist_https_connection *conn = (cwist_https_connection *)a->https_conn;
        cwist_error_t err = (a->req && a->req->method == CWIST_HTTP_HEAD)
                                ? cwist_https_send_response_head(conn, res)
                                : cwist_https_send_response(conn, res);
        bool ok = cwist_error_is_ok(&err);

        if (keep && ok && a->app && a->app->ssl_ctx && a->app->https_request_handler) {
            conn->deferred = false;
            https_pool_submit_conn(conn, a->app->ssl_ctx, a->app->https_request_handler, a->app);
        } else {
            cwist_https_close_connection(conn);
        }
    } else if (a->orphaned) {
        /* The server stopped and destroyed the worker reactor this
         * connection was parked on: nothing can send on it or re-arm it any
         * more, so close it. */
        cwist_http_async_close_orphan(a->client_fd, a->conn);
    } else if (a->reactor) {
        /* Resumable write: on a partial send the remainder is parked on a
         * POLLOUT slot and the reactor thread is freed immediately; the
         * parked callback performs the rearm/close when the drain ends. */
        cwist_http_async_send_response(a->client_fd, res, a->reactor, a->conn, keep,
                                       a->req && a->req->method == CWIST_HTTP_HEAD);
    } else {
        cwist_error_t err = (a->req && a->req->method == CWIST_HTTP_HEAD)
                                ? cwist_http_send_response_head(a->client_fd, res)
                                : cwist_http_send_response(a->client_fd, res);
        bool ok = err.error.err_i16 == 0;

        if (keep && ok && a->app) {
            cwist_http_pool_rearm_current(a->client_fd, cwist_app_http_handler, a->app);
        } else {
            close(a->client_fd);
        }
    }

    /* See the file header: never free before dispatch observed the defer. */
    while (!atomic_load_explicit(&a->ack, memory_order_acquire)) sched_yield();

    if (a->final_res_owned) cwist_http_response_destroy(res);
    cwist_http_response_destroy(a->res);
    cwist_http_request_destroy(a->req);
    cwist_async_release(a);
}

static void cwist_async_reactor_complete(void *ctx) {
    cwist_async_complete((cwist_async *)ctx);
}

/* Producer side, called with mu held after the response is final: detach
 * what the producer allocated from its own GC scope, then decide who sends.
 * Returns true if the caller must deliver (after unlocking); false if the
 * ack has not happened yet and will deliver. */
static bool cwist_async_publish_locked(cwist_async *a) {
    /* Runs on the producer, before a reactor/H2 consumer can see or free
     * newly allocated body/header/string state (also timeout and abort). */
    cwist_http_async_disown_response(a->final_res);
    if (!atomic_load_explicit(&a->ack, memory_order_acquire)) {
        /* Dispatch has not unwound yet (or this is the creator completing
         * inline): record the completion; the ack sends it. Needs no
         * allocation, so an abort still works when scheduling failed. */
        a->finish_on_ack = true;
        return false;
    }
    return true;
}

/* Send the completed exchange: post it to the owning reactor, or finish
 * inline on the classic/TLS/orphaned paths. Runs exactly once, on the
 * producer or on the dispatch ack, never under mu. */
static void cwist_async_deliver(cwist_async *a) {
    if (a->reactor) {
        if (cwist_http_reactor_post_live(a->reactor, a->reactor_gen, &a->post)) return;
        /* The pool that owned the reactor has been destroyed (the server
         * stopped while this exchange was pending): the reactor is freed, so
         * close out inline without it. */
        a->orphaned = true;
    }
    cwist_async_complete(a);
}

/* --- Timeout ------------------------------------------------------------- */

static pthread_once_t g_timeout_once = PTHREAD_ONCE_INIT;
static cwist_scheduler_t *g_timeout_sched;

static void cwist_async_timeout_sched_init(void) {
    g_timeout_sched = cwist_scheduler_create(1, 64);
}

static void cwist_async_timeout_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    if (!cwist_async_claim(a)) {
        cwist_async_release(a); /* A real response beat the timeout. */
        return;
    }
    pthread_mutex_lock(&a->mu);
    cwist_http_response *res = a->res;
    res->status_code = CWIST_HTTP_GATEWAY_TIMEOUT;
    cwist_sstring_assign(res->status_text, (char *)"Gateway Timeout");
    cwist_sstring_assign(res->body, (char *)"Gateway Timeout");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    bool deliver = cwist_async_publish_locked(a);
    pthread_mutex_unlock(&a->mu);
    if (deliver) cwist_async_deliver(a);
    cwist_async_release(a);
}

void cwist_async_set_timeout(cwist_async *a, uint64_t ms) {
    if (!a || ms == 0) return;
    pthread_once(&g_timeout_once, cwist_async_timeout_sched_init);
    if (!g_timeout_sched) return;
    cwist_async_retain(a);
    if (!cwist_scheduler_schedule(g_timeout_sched, cwist_async_timeout_job, a, ms)) {
        cwist_async_release(a);
    }
}

/* --- Completion API ------------------------------------------------------ */

bool cwist_async_respond(cwist_async *a, cwist_http_status_t status, const char *content_type,
                         const void *body, size_t len) {
    if (!a || !cwist_async_claim(a)) return false;
    pthread_mutex_lock(&a->mu);
    cwist_http_response *res = a->res;
    res->status_code = status;
    cwist_sstring_assign(res->status_text, (char *)cwist_async_reason(status));
    if (body && len > 0) {
        cwist_sstring_assign_len(res->body, (const char *)body, len);
    }
    if (content_type) {
        cwist_http_header_add(&res->headers, "Content-Type", content_type);
    }
    bool deliver = cwist_async_publish_locked(a);
    pthread_mutex_unlock(&a->mu);
    if (deliver) cwist_async_deliver(a);
    return true;
}

bool cwist_async_respond_with(cwist_async *a, cwist_http_response *res) {
    if (!a || !res || !cwist_async_claim(a)) return false;
    pthread_mutex_lock(&a->mu);
    a->final_res = res;
    a->final_res_owned = res != a->res;
    bool deliver = cwist_async_publish_locked(a);
    pthread_mutex_unlock(&a->mu);
    if (deliver) cwist_async_deliver(a);
    return true;
}

bool cwist_async_abort(cwist_async *a, cwist_http_status_t status) {
    if (!a || !cwist_async_claim(a)) return false;
    pthread_mutex_lock(&a->mu);
    cwist_http_response *res = a->res;
    res->status_code = status;
    const char *reason = cwist_async_reason(status);
    cwist_sstring_assign(res->status_text, (char *)reason);
    cwist_sstring_assign(res->body, (char *)reason);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    a->keep_alive = false;
    bool deliver = cwist_async_publish_locked(a);
    pthread_mutex_unlock(&a->mu);
    if (deliver) cwist_async_deliver(a);
    return true;
}
