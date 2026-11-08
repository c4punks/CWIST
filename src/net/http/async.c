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
 * The handoff is lock-free and rests on two rules:
 *
 *  - Single writer. Until the ack only the dispatch thread touches req/res
 *    (GC disown, keep-alive) and the dispatch-side fields; the producer that
 *    wins the claim writes only its own result fields (r_*, final_res).
 *    respond()/abort()/timeout record their outcome there instead of editing
 *    res, and the deliverer applies it to res once both sides are done.
 *  - One state word. Dispatch sets ACKED and the winner sets PUBLISHED with
 *    acq_rel fetch_or on @c st; whichever arrives second sees the other's bit,
 *    has acquired the other's writes, and delivers. Exactly one side does.
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
#include <stdint.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <cwist/sys/platform.h>
#ifndef CWIST_BAREMETAL
#include <sys/socket.h>
#endif
#include <unistd.h>

/* Bits of cwist_async.st. */
enum {
    CWIST_ASYNC_CLAIMED = 1u << 0,   /* A producer won the right to complete. */
    CWIST_ASYNC_PUBLISHED = 1u << 1, /* The winner's result is fully written. */
    CWIST_ASYNC_ACKED = 1u << 2,     /* Dispatch released req/res. */
    CWIST_ASYNC_NOKEEP = 1u << 3,    /* The winner refused keep-alive (abort). */
};

struct cwist_async {
    _Atomic size_t refs;          /* Completion plus retained producers/timers. */
    _Atomic unsigned st;          /* CWIST_ASYNC_* bits; see the file header. */
    pthread_t dispatch_thread;
    cwist_http_request *req;
    cwist_http_response *res;     /* Handler's response (request arena). */
    /* Dispatch-only until ACKED. */
    bool keep_alive;              /* Dispatch's view; the winner's is NOKEEP. */
    cwist_http_carry *carry;      /* Classic-pool pipelined surplus (malloc). */
    /* Claim winner only until PUBLISHED. */
    cwist_http_response *final_res;
    bool final_res_owned;         /* final_res came from respond_with. */
    bool streamed;                /* Response streamed incrementally by winner. */
    bool stream_ok;               /* Stream completed without write errors. */
    bool r_apply;                 /* Apply r_* onto res at delivery. */
    cwist_http_status_t r_status;
    const char *r_body;           /* NULL keeps the handler's body. */
    size_t r_len;
    const char *r_ctype;          /* NULL adds no Content-Type. */
    void *r_block;                /* Owns copies of r_body/r_ctype (disowned). */
    int client_fd;
    struct cwist_app *app;
    cwist_reactor_t *reactor;     /* NULL on the classic pool path. */
    uint64_t reactor_gen;         /* Pool generation @c reactor belongs to. */
    bool orphaned;                /* @c reactor was destroyed before completion. */
    cwist_http_async_conn_t *conn;
    void *https_conn;             /* cwist_https_connection * on TLS path. */
    cwist_h2_async_queue *h2_queue; /* Per-connection queue on the H2 path. */
    uint32_t h2_stream_id;
    struct cwist_h3_async_queue *h3_queue; /* Engine queue on the HTTP/3 path. */
    struct cwist_h3_pending *h3_pending;   /* Stream token on the HTTP/3 path. */
    cwist_reactor_post_t post;
};

static void cwist_async_reactor_complete(void *ctx);
static void cwist_async_deliver(cwist_async *a);

static bool cwist_async_acked(cwist_async *a) {
    return (atomic_load_explicit(&a->st, memory_order_acquire) & CWIST_ASYNC_ACKED) != 0;
}

static void cwist_async_wait_ack(cwist_async *a) {
    while (!cwist_async_acked(a)) sched_yield();
}

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

#ifdef CWIST_HTTP3
static __thread struct {
    cwist_http_request *req;
    struct cwist_h3_async_queue *q;
    struct cwist_h3_pending *p;
    bool deferred;
} t_h3_target;

void cwist_async_h3_begin(cwist_http_request *req, struct cwist_h3_async_queue *q,
                          struct cwist_h3_pending *p) {
    t_h3_target.req = req;
    t_h3_target.q = q;
    t_h3_target.p = p;
    t_h3_target.deferred = false;
}

bool cwist_async_h3_end(void) {
    bool deferred = t_h3_target.deferred;
    memset(&t_h3_target, 0, sizeof(t_h3_target));
    return deferred;
}
#endif

bool cwist_async_https_take_deferred(void) {
    bool deferred = t_https_deferred;
    t_https_deferred = false;
    return deferred;
}

cwist_async *cwist_async_defer(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || res->deferred) return NULL;
#ifdef CWIST_HTTP3
    /* An HTTP/3 stream with no engine queue has no completion path: refuse,
     * so the handler answers inline instead of a completion writing to a
     * descriptor that is not this stream's. */
    if (t_h3_target.req == req && (!t_h3_target.q || !t_h3_target.p)) return NULL;
#endif
    cwist_async *a = cwist_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    atomic_init(&a->refs, 1);
    atomic_init(&a->st, 0u);
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
#ifdef CWIST_HTTP3
    if (t_h3_target.req == req && t_h3_target.q && t_h3_target.p) {
        /* Both references outlive the engine loop and the stream; the
         * completion hands them to the queue. */
        a->h3_queue = cwist_h3_async_queue_acquire(t_h3_target.q);
        a->h3_pending = cwist_h3_pending_acquire(t_h3_target.p);
        t_h3_target.deferred = true;
    }
#endif
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
    cwist_async_record_pending(a, res);
    return a;
}

cwist_async *cwist_async_retain(cwist_async *a) {
    if (a) atomic_fetch_add_explicit(&a->refs, 1, memory_order_relaxed);
    return a;
}

void cwist_async_release(cwist_async *a) {
    if (a && atomic_fetch_sub_explicit(&a->refs, 1, memory_order_acq_rel) == 1) {
        cwist_async_forget_handle(a);
        free(a->carry);
        cwist_free(a->r_block);
#ifdef CWIST_HTTP3
        /* A handle released without completing (cannot happen through the
         * public API, which always completes once) still drops its refs. */
        cwist_h3_pending_release(a->h3_pending);
        cwist_h3_async_queue_release(a->h3_queue);
#endif
        cwist_free(a);
    }
}

void cwist_async_set_carry(cwist_async *a, const char *bytes, size_t len) {
    if (!a || !bytes || len == 0) return;
    /* Dispatch thread, before the ack: carry and keep_alive are dispatch-only
     * until then and the ack's release publishes them, so no lock. */
    /* malloc, not cwist_alloc: the completion thread frees it, so it must not
     * belong to the dispatch thread's GC scope. */
    cwist_http_carry *c = (cwist_http_carry *)malloc(sizeof(*c) + len);
    if (!c) {
        a->keep_alive = false;
        return;
    }
    c->app = a->app;
    c->len = len;
    memcpy(c->bytes, bytes, len);
    free(a->carry);
    a->carry = c;
}

void cwist_async_dispatch_ack(cwist_async *a) {
    if (!a) return;
    cwist_async_forget_handle(a);
    /* Middleware may allocate after next() returns. Producers never touch
     * this graph (they write only their own result fields), so detaching it
     * from this thread's GC needs no lock. */
    cwist_http_async_disown_request(a->req);
    cwist_http_async_disown_response(a->res);
    a->keep_alive = a->keep_alive && a->req->keep_alive && a->res->keep_alive;
    unsigned old = atomic_fetch_or_explicit(&a->st, CWIST_ASYNC_ACKED, memory_order_acq_rel);
    /* Without a published completion a foreign producer may free a from
     * here on; with one, this thread arrived second and sends it. */
    if (old & CWIST_ASYNC_PUBLISHED) cwist_async_deliver(a);
}

void cwist_async_wait_dispatched(cwist_async *a) {
    if (!a || pthread_equal(a->dispatch_thread, pthread_self())) return;
    cwist_async_wait_ack(a);
}

static bool cwist_async_claim(cwist_async *a) {
    unsigned old = atomic_fetch_or_explicit(&a->st, CWIST_ASYNC_CLAIMED, memory_order_acq_rel);
    return (old & CWIST_ASYNC_CLAIMED) == 0;
}

/* Claim winner, after its result fields are written: publish them and learn
 * whether dispatch already acked. True means this thread arrived second and
 * must deliver; false means the ack will. */
static bool cwist_async_publish(cwist_async *a, bool keep) {
    unsigned set = CWIST_ASYNC_PUBLISHED | (keep ? 0u : CWIST_ASYNC_NOKEEP);
    unsigned old = atomic_fetch_or_explicit(&a->st, set, memory_order_acq_rel);
    return (old & CWIST_ASYNC_ACKED) != 0;
}

/* Record a status/body/Content-Type outcome for delivery to apply. @p body and
 * @p ctype must outlive the handle (static) unless @p block owns them. */
static void cwist_async_set_result(cwist_async *a, cwist_http_status_t status, const char *body,
                                   size_t len, const char *ctype, void *block) {
    a->r_apply = true;
    a->r_status = status;
    a->r_body = body;
    a->r_len = len;
    a->r_ctype = ctype;
    a->r_block = block;
}

/* Deliverer, with both PUBLISHED and ACKED seen: the graph has one owner
 * again, so the recorded outcome can be written into res. */
static void cwist_async_apply_result(cwist_async *a) {
    if (!a->r_apply) return;
    a->r_apply = false;
    cwist_http_response *res = a->res;
    res->status_code = a->r_status;
    cwist_sstring_assign(res->status_text, (char *)cwist_async_reason(a->r_status));
    if (a->r_body && a->r_len > 0) cwist_sstring_assign_len(res->body, a->r_body, a->r_len);
    if (a->r_ctype) cwist_http_header_add(&res->headers, "Content-Type", a->r_ctype);
    cwist_free(a->r_block);
    a->r_block = NULL;
    /* Those strings and headers belong to this thread's GC scope; the
     * consumer may run elsewhere. */
    cwist_http_async_disown_response(res);
}

static void cwist_async_reactor_complete(void *ctx);

/* Send the final response, re-arm or close the connection, then release the
 * request/response pair and the handle itself. */
static void cwist_async_complete(cwist_async *a) {
    cwist_http_response *res = a->final_res;
    bool keep = a->keep_alive &&
                !(atomic_load_explicit(&a->st, memory_order_acquire) & CWIST_ASYNC_NOKEEP) &&
                atomic_load(&g_cwist_running);
    res->keep_alive = keep;

    if (a->h2_queue) {
        /* Plan B handoff: never write frames from a worker thread.  Enqueue
         * the finished exchange and poke the connection's wake fd; the
         * connection thread drains the queue, HPACK-encodes, and sends.
         * Wait for the dispatch ack first: enqueueing transfers req/res
         * ownership, which is only legal once dispatch observed the defer. */
        cwist_async_wait_ack(a);
        cwist_h2_async_queue_enqueue(a->h2_queue, a->h2_stream_id, a->req, a->final_res, a->res,
                                     a->final_res_owned);
        cwist_h2_async_queue_release(a->h2_queue);
        cwist_async_release(a);
        return;
    }

#ifdef CWIST_HTTP3
    if (a->h3_queue) {
        /* Same rule as HTTP/2: the engine thread alone writes the stream.
         * Enqueueing transfers req/res, legal only after the ack. */
        cwist_async_wait_ack(a);
        struct cwist_h3_pending *p = a->h3_pending;
        a->h3_pending = NULL;
        cwist_h3_async_queue_enqueue(a->h3_queue, p, a->req, a->final_res, a->res,
                                     a->final_res_owned);
        cwist_h3_async_queue_release(a->h3_queue);
        a->h3_queue = NULL;
        cwist_async_release(a);
        return;
    }
#endif

    if (a->https_conn) {
        cwist_https_connection *conn = (cwist_https_connection *)a->https_conn;
        bool ok;
        if (a->streamed) {
            ok = a->stream_ok;
        } else {
            cwist_error_t err = (a->req && a->req->method == CWIST_HTTP_HEAD)
                                    ? cwist_https_send_response_head(conn, res)
                                    : cwist_https_send_response(conn, res);
            ok = cwist_error_is_ok(&err);
        }

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
        if (a->streamed) {
            cwist_http_async_finish_stream(a->client_fd, a->reactor, a->conn,
                                           keep && a->stream_ok);
        } else {
            /* Resumable write: on a partial send the remainder is parked on a
             * POLLOUT slot and the reactor thread is freed immediately; the
             * parked callback performs the rearm/close when the drain ends. */
            cwist_http_async_send_response(a->client_fd, res, a->reactor, a->conn, keep,
                                           a->req && a->req->method == CWIST_HTTP_HEAD);
        }
    } else {
        bool ok;
        if (a->streamed) {
            ok = a->stream_ok;
        } else {
            cwist_error_t err = (a->req && a->req->method == CWIST_HTTP_HEAD)
                                    ? cwist_http_send_response_head(a->client_fd, res)
                                    : cwist_http_send_response(a->client_fd, res);
            ok = err.error.err_i16 == 0;
        }

        if (keep && ok && a->app) {
            if (a->carry) {
                /* Pipelined requests read before the defer resume first. */
                cwist_http_carry *carry = a->carry;
                a->carry = NULL;
                cwist_http_pool_rearm_current(a->client_fd, cwist_app_http_handler_resume, carry);
            } else {
                cwist_http_pool_rearm_current(a->client_fd, cwist_app_http_handler, a->app);
            }
        } else {
            close(a->client_fd);
        }
    }

    /* See the file header: never free before dispatch observed the defer. */
    cwist_async_wait_ack(a);

    if (a->final_res_owned) cwist_http_response_destroy(res);
    cwist_http_response_destroy(a->res);
    cwist_http_request_destroy(a->req);
    cwist_async_release(a);
}

static void cwist_async_reactor_complete(void *ctx) {
    cwist_async_complete((cwist_async *)ctx);
}

/* Send the completed exchange: post it to the owning reactor, or finish
 * inline on the classic/TLS/orphaned paths. Runs exactly once, on whichever
 * of the producer and the dispatch ack arrived second. */
static void cwist_async_deliver(cwist_async *a) {
    cwist_async_apply_result(a);
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
    const char *reason = cwist_async_reason(CWIST_HTTP_GATEWAY_TIMEOUT);
    cwist_async_set_result(a, CWIST_HTTP_GATEWAY_TIMEOUT, reason, strlen(reason), "text/plain",
                           NULL);
    if (cwist_async_publish(a, true)) cwist_async_deliver(a);
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
    /* The caller's buffers may not outlive this call: copy them into one
     * block that delivery applies and frees, possibly on another thread. */
    size_t blen = (body && len > 0) ? len : 0;
    size_t clen = content_type ? strlen(content_type) + 1 : 0;
    bool fits = blen <= SIZE_MAX - clen;
    size_t total = fits ? blen + clen : 0;
    char *block = total > 0 ? (char *)cwist_alloc(total) : NULL;
    if (!fits || (total > 0 && !block)) {
        /* Out of memory: still complete exactly once, without allocating. */
        const char *reason = cwist_async_reason(CWIST_HTTP_INTERNAL_ERROR);
        cwist_async_set_result(a, CWIST_HTTP_INTERNAL_ERROR, reason, strlen(reason), "text/plain",
                               NULL);
        if (cwist_async_publish(a, false)) cwist_async_deliver(a);
        return true;
    }
    if (block) {
        cwist_gc_scope_disown(block);
        if (blen) memcpy(block, body, blen);
        if (clen) memcpy(block + blen, content_type, clen);
    }
    cwist_async_set_result(a, status, blen ? block : NULL, blen, clen ? block + blen : NULL, block);
    if (cwist_async_publish(a, true)) cwist_async_deliver(a);
    return true;
}

bool cwist_async_respond_with(cwist_async *a, cwist_http_response *res) {
    if (!a || !res || !cwist_async_claim(a)) return false;
    a->final_res = res;
    a->final_res_owned = res != a->res;
    /* Before publishing: a consumer elsewhere may free what this thread
     * allocated into res. */
    cwist_http_async_disown_response(res);
    if (cwist_async_publish(a, true)) cwist_async_deliver(a);
    return true;
}

bool cwist_async_abort(cwist_async *a, cwist_http_status_t status) {
    if (!a || !cwist_async_claim(a)) return false;
    /* Needs no allocation, so an abort still works when scheduling failed. */
    const char *reason = cwist_async_reason(status);
    cwist_async_set_result(a, status, reason, strlen(reason), "text/plain", NULL);
    if (cwist_async_publish(a, false)) cwist_async_deliver(a);
    return true;
}

bool cwist_async_can_stream(const cwist_async *a) {
    if (!a || a->h2_queue) return false;
#ifdef CWIST_HTTP3
    if (a->h3_queue) return false;
#endif
    return a->https_conn != NULL || a->client_fd >= 0;
}

bool cwist_async_is_streamed(const cwist_async *a) {
    return a && a->streamed;
}

bool cwist_async_stream_start(cwist_async *a, cwist_http_response *res, bool chunked) {
    if (!cwist_async_can_stream(a) || !res) return false;
    cwist_async_wait_dispatched(a);
    if (!cwist_async_claim(a)) return false;
    a->streamed = true;
    a->stream_ok = false;
    a->final_res = res;
    a->final_res_owned = res != a->res;
    cwist_http_async_disown_response(res);

    bool keep = a->keep_alive && atomic_load(&g_cwist_running);
    res->keep_alive = keep;
    res->stream_mode = chunked;

    if (a->https_conn) {
        cwist_https_connection *conn = (cwist_https_connection *)a->https_conn;
        cwist_error_t err = cwist_https_send_response_head(conn, res);
        a->stream_ok = cwist_error_is_ok(&err);
    } else {
        cwist_error_t err = cwist_http_send_response_head(a->client_fd, res);
        a->stream_ok = (err.error.err_i16 == 0);
    }
    return a->stream_ok;
}

bool cwist_async_stream_write(cwist_async *a, const void *data, size_t len, bool chunked) {
    if (!a || !a->streamed || !a->stream_ok) return false;
    if (len == 0) return true;
    if (!data) {
        a->stream_ok = false;
        return false;
    }
    if (a->https_conn) {
        cwist_https_connection *conn = (cwist_https_connection *)a->https_conn;
        if (chunked) {
            char hdr[24];
            int hlen = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
            if (hlen <= 0 || (size_t)hlen >= sizeof(hdr) ||
                cwist_https_write_raw(conn, hdr, (size_t)hlen) != 0 ||
                cwist_https_write_raw(conn, data, len) != 0 ||
                cwist_https_write_raw(conn, "\r\n", 2) != 0) {
                a->stream_ok = false;
                return false;
            }
        } else if (cwist_https_write_raw(conn, data, len) != 0) {
            a->stream_ok = false;
            return false;
        }
    } else {
        int rc = chunked ? cwist_http_write_chunk(a->client_fd, data, len)
                         : cwist_http_write_raw(a->client_fd, data, len);
        if (rc != 0) {
            a->stream_ok = false;
            return false;
        }
    }
    return true;
}

void cwist_async_stream_finish(cwist_async *a, bool ok, bool chunked) {
    if (!a || !a->streamed) return;
    bool final_ok = ok && a->stream_ok;
    if (final_ok && chunked) {
        if (a->https_conn) {
            cwist_https_connection *conn = (cwist_https_connection *)a->https_conn;
            if (cwist_https_write_raw(conn, "0\r\n\r\n", 5) != 0) final_ok = false;
        } else {
            if (cwist_http_write_raw(a->client_fd, "0\r\n\r\n", 5) != 0) final_ok = false;
        }
    }
    a->stream_ok = final_ok;
    if (cwist_async_publish(a, final_ok)) cwist_async_deliver(a);
}
