/**
 * @file compat.c
 * @brief WASI (preview1) compatibility stubs for server-only subsystems.
 *
 * The socket server runtime (reactor, parked writers, metrics) compiles into
 * the WASI archive because request dispatch shares translation units with
 * it, but a WASM host never binds sockets: the stubbed entry points are
 * reachable at link time yet never executed. This mirrors how the
 * Emscripten target satisfies the same references - through its system
 * stub libraries (-lstubs, -lsockets) plus binaryen dead-code elimination -
 * but expressed as plain C so the WASI link needs no post-processing.
 *
 * pthread_* symbols come from wasi-sdk's libwasi-emulated-pthread; link
 * with -lwasi-emulated-pthread.
 */

#if defined(__wasi__)

#include <cwist/sys/wasi.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/sys/metrics/metrics.h>
#include <cwist/net/http/writer_fast.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/net/grpc/grpc.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/* --- Reactor: no event loop exists outside the process runtime. --------- */

cwist_reactor_t *cwist_reactor_create(void) {
    return NULL;
}

bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, const void *payload,
                       size_t payload_size) {
    (void)reactor;
    (void)fd;
    (void)cb;
    (void)payload;
    (void)payload_size;
    return false;
}

bool cwist_reactor_del(cwist_reactor_t *reactor, int fd) {
    (void)reactor;
    (void)fd;
    return false;
}

void cwist_reactor_run(cwist_reactor_t *reactor) {
    (void)reactor;
}

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, const void *payload,
                       size_t payload_size) {
    (void)reactor;
    (void)fd;
    (void)cb;
    (void)payload;
    (void)payload_size;
    return false;
}

bool cwist_reactor_add_out(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                           const void *payload, size_t payload_size) {
    (void)reactor;
    (void)fd;
    (void)cb;
    (void)payload;
    (void)payload_size;
    return false;
}

bool cwist_reactor_post(cwist_reactor_t *reactor, cwist_reactor_post_t *node) {
    (void)reactor;
    (void)node;
    return false;
}

void cwist_reactor_stop(cwist_reactor_t *reactor) {
    (void)reactor;
}

void cwist_reactor_destroy(cwist_reactor_t *reactor) {
    (void)reactor;
}

/* --- Metrics / parked-writer fast paths: under WASI 0.2 the real
 * metrics.c and writer_fast.c join the build, so these stubs exist only
 * for preview1 where those units cannot compile. ------------------------ */
#ifndef CWIST_WASI_SOCKETS
static cwist_metrics_registry_t *const g_wasi_metrics_sink =
    (cwist_metrics_registry_t *)(uintptr_t)1;

cwist_metrics_registry_t *cwist_metrics_registry(void) {
    return g_wasi_metrics_sink;
}

void cwist_metric_inc(cwist_metrics_registry_t *reg, cwist_metric_id_t id) {
    (void)reg;
    (void)id;
}

uint32_t cwist_fast_monotonic_sec(void) {
    return 0;
}

cwist_write_status_t cwist_http_sendmsg_speculative(int fd, struct iovec *iov, int iovcnt,
                                                    int flags, size_t *total_sent) {
    (void)fd;
    (void)iov;
    (void)iovcnt;
    (void)flags;
    if (total_sent) *total_sent = 0;
    return CWIST_WRITE_ERR;
}
#endif /* !CWIST_WASI_SOCKETS */

/* --- HTTPS/HTTP-2 upgrade paths: no TLS in a WASM host. ----------------- */

bool cwist_https_connection_uses_http2(const cwist_https_connection *conn) {
    (void)conn;
    return false;
}

cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    (void)conn;
    return NULL;
}

cwist_error_t cwist_http2_serve_connection_ex(cwist_https_connection *conn, void *user_ctx,
                                              cwist_http2_request_handler_func handler,
                                              const cwist_http2_stream_hooks *hooks) {
    (void)conn;
    (void)user_ctx;
    (void)handler;
    (void)hooks;
    cwist_error_t err = {0};
    err.error.err_i16 = -1;
    return err;
}

const cwist_http2_stream_hooks *cwist_grpc_http2_hooks(void) {
    return NULL;
}

/* H2 deferred-completion queue: reachable through async.c's Plan B handoff,
 * but a WASM host never runs an H2 connection, so req->h2_queue is always
 * NULL on this target and the stubs are never executed. */
cwist_h2_async_queue *cwist_h2_async_queue_acquire(cwist_h2_async_queue *q) {
    return q;
}

void cwist_h2_async_queue_release(cwist_h2_async_queue *q) {
    (void)q;
}

int cwist_h2_async_queue_enqueue(cwist_h2_async_queue *q, uint32_t stream_id,
                                 cwist_http_request *req, cwist_http_response *send,
                                 cwist_http_response *res, bool send_owned) {
    (void)q;
    (void)stream_id;
    (void)req;
    (void)send;
    (void)res;
    (void)send_owned;
    return -1;
}

/* HTTPS connection send/close: reachable through async.c's completion path,
 * never executed -- a WASM host dispatches in memory and never wraps a TLS
 * connection. */
void cwist_https_close_connection(cwist_https_connection *conn) {
    (void)conn;
}

static cwist_error_t cwist_wasi_https_unavailable(void) {
    cwist_error_t err = {0};
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    (void)conn;
    (void)res;
    return cwist_wasi_https_unavailable();
}

cwist_error_t cwist_https_send_response_head(cwist_https_connection *conn,
                                             cwist_http_response *res) {
    (void)conn;
    (void)res;
    return cwist_wasi_https_unavailable();
}

void https_pool_submit_conn(cwist_https_connection *conn, cwist_https_context *ctx,
                            void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    (void)conn;
    (void)ctx;
    (void)handler;
    (void)user_ctx;
}

#endif /* __wasi__ */
