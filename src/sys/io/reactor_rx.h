/* Internal RX-uring receive path (issue #179).
 *
 * The C1M async HTTP server normally waits for readability with a one-shot
 * POLL_ADD and then drains the fd with recv().  When the reactor runs a real
 * io_uring ring, the receive wait can instead be an IORING_OP_RECV whose
 * completion IS the readiness signal: one SQE replaces the POLL+recv pair
 * and rides the existing deferred-SQE batch, so no extra io_uring_enter is
 * paid beyond what the run loop already spends.
 *
 * SQE namespace: the run loop dispatches on cqe->user_data, historically an
 * ev_ctx pointer.  RECV SQEs tag the low bit of user_data and carry the
 * connection pointer in the remaining bits:
 *
 *     user_data = (uint64_t)conn | 1
 *
 * ev_ctx pointers come from cwist_alloc (malloc-backed, so at least
 * max_align_t aligned) and can never carry the tag bit; reactor.c asserts
 * this when arming.
 *
 * RX-epoll: on an epoll reactor (no io_uring ring, e.g. under a seccomp
 * profile that blocks it) the same contract is served without SQEs.  The fd
 * stays registered edge-triggered (no EPOLLONESHOT) with user_data
 * fd << 1 | 1, and on readiness the reactor itself recv()s into the armed
 * buffer and reports the result through the same completion handler.  A
 * steady keep-alive request then costs one recv and one send, where the
 * one-shot path also paid an epoll_ctl re-arm.  Epoll arms are accepted on
 * the run thread only; elsewhere cwist_reactor_recv_arm returns false and the
 * caller's POLL fallback re-arms from the run thread.  Any cwist_reactor_add,
 * _mod or _del of the fd takes it back to the one-shot path.  -EAGAIN is
 * never reported: the receive stays armed until bytes, EOF or an error.
 *
 * This header is private to the build (src/sys/io); it is not installed.
 * Everything compiles to no-ops on non-Linux so callers need no ifdefs
 * beyond cheap inline checks. */
#ifndef CWIST_SYS_IO_REACTOR_RX_H
#define CWIST_SYS_IO_REACTOR_RX_H

#include <cwist/sys/io/reactor.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __linux__

/* CWIST_RX_URING env gate: unset = auto (enabled), "1" = force attempt,
 * "0" = force off (legacy POLL path).  Cached after the first read.  This is
 * only the knob; per-reactor usability still requires an io_uring ring (see
 * cwist_reactor_rx_supported). */
bool cwist_rx_uring_env_enabled(void);

/* True iff this reactor can take the RX path: an io_uring reactor with
 * CWIST_RX_URING allowing it, or an epoll reactor with CWIST_RX_EPOLL
 * allowing it. */
bool cwist_reactor_rx_supported(const cwist_reactor_t *reactor);

/* Drop the epoll registration state recorded for fd.  Call on a freshly
 * accepted fd: its number may have belonged to a closed socket, and the
 * stale state would otherwise cost the first re-arm a failed epoll_ctl (and
 * must never let an RX arm for a new connection skip registration). */
void cwist_reactor_fd_forget(int fd);

/* Completion handler for tagged RECV SQEs.  conn is the untagged connection
 * pointer; res is the CQE result: byte count, 0 (EOF), -EAGAIN, or another
 * negative errno.  Invoked on the reactor owner thread while dispatching. */
typedef void (*cwist_rx_cb_t)(void *conn, int res);

/* Register the completion handler (idempotent; http.c sets it once per
 * worker reactor right after cwist_reactor_create). */
void cwist_reactor_set_rx_cb(cwist_reactor_t *reactor, cwist_rx_cb_t cb);

/* Arm one IORING_OP_RECV on the reactor ring.  The SQE references
 * [buf, buf+len) and must stay valid until the completion arrives, so the
 * caller must not move or consume that buffer while the SQE is in flight.
 * At most one RECV per connection at a time.  When the latency probe is
 * enabled, *armed_ns (if non-NULL) receives the arm timestamp so the caller
 * can compute queue delay.  Run-thread calls during dispatch defer into the
 * existing SQE batch; other threads submit immediately so parked workers
 * wake.  Returns false when unsupported or on submission failure; the caller
 * then falls back to the legacy POLL path. */
bool cwist_reactor_recv_arm(cwist_reactor_t *reactor, int fd, void *buf, unsigned len, void *conn,
                            uint64_t *armed_ns);

/* Record one latency-probe sample (queue=true for queue_delay, false for
 * svc) on the reactor's probe histograms.  No-op unless the probe is
 * enabled; used by the RX path, whose dispatch bypasses the ev_ctx probe
 * wrapper in cwist_reactor_run. */
void cwist_reactor_probe_record(cwist_reactor_t *reactor, bool queue, uint64_t sample_us);

/* True while the calling thread dispatches a reactor callback in late mode:
 * the round it belongs to has run past CWIST_REACTOR_LATE_US (issue #319
 * experiment), so the connection should finish its buffered work now
 * instead of yielding. False off the run thread and outside dispatch. */
bool cwist_reactor_dispatch_late(void);

#else /* !__linux__ */

static inline bool cwist_reactor_dispatch_late(void) {
    return false;
}

static inline bool cwist_rx_uring_env_enabled(void) {
    return false;
}
static inline bool cwist_reactor_rx_supported(const cwist_reactor_t *reactor) {
    (void)reactor;
    return false;
}
static inline void cwist_reactor_fd_forget(int fd) {
    (void)fd;
}
typedef void (*cwist_rx_cb_t)(void *conn, int res);
static inline void cwist_reactor_set_rx_cb(cwist_reactor_t *reactor, cwist_rx_cb_t cb) {
    (void)reactor;
    (void)cb;
}
static inline bool cwist_reactor_recv_arm(cwist_reactor_t *reactor, int fd, void *buf, unsigned len,
                                          void *conn, uint64_t *armed_ns) {
    (void)reactor;
    (void)fd;
    (void)buf;
    (void)len;
    (void)conn;
    (void)armed_ns;
    return false;
}
static inline void cwist_reactor_probe_record(cwist_reactor_t *reactor, bool queue,
                                              uint64_t sample_us) {
    (void)reactor;
    (void)queue;
    (void)sample_us;
}

#endif /* __linux__ */

#ifdef __cplusplus
}
#endif

#endif /* CWIST_SYS_IO_REACTOR_RX_H */
