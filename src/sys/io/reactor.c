/**
 * @file reactor.c
 * @brief Readiness multiplexer (io_uring / epoll / kqueue) driving CWIST's
 * synchronous callback model.
 *
 * Design note - why readiness notification + synchronous completion is kept:
 * 1. No queues: a request passes through no queue at all; the woken worker
 *    finishes it immediately. This is the source of the 0.0x ms latency.
 *    A completion model pushes each request through a ring 3-4 times and
 *    binds progress to loop ticks, landing at 2-3ms (Axum/Tokio level).
 * 2. Structural backpressure: callbacks block, so unfinished work cannot
 *    pile up in kernel or userland. A single in-flight cap per thread
 *    (32 in http.c) is all the flow control the server needs.
 * 3. Cache locality: the whole request lifetime runs on one thread's
 *    contiguous stack, reusing L1/L2. A completion model splits the
 *    handler into fragments and lifts per-stage state onto the heap.
 * 4. No state machines: handlers are straight-line code; a stack trace
 *    is the request's execution history.
 * 5. Deterministic tail: with no queue waiting, p99/p999 converge on the
 *    mean.
 * The cost: per-connection concurrency is bounded by worker count
 * (cores*8), covered by multi-process scaling (fork + SO_REUSEPORT).
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/reactor.h>
#include "reactor_rx.h"
#include "async_debug.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/io_uring.h>
#if defined(__has_include)
#if __has_include(<linux/time_types.h>)
#include <linux/time_types.h>
#endif
#endif
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
struct io_uring_getevents_arg {
    uint64_t sigmask;
    uint32_t sigmask_sz;
    uint32_t pad;
    uint64_t ts;
};
#endif

#ifndef IORING_ASYNC_CANCEL_ALL
#define IORING_ASYNC_CANCEL_ALL (1U << 0)
#endif

#ifndef IORING_ASYNC_CANCEL_FD
#define IORING_ASYNC_CANCEL_FD (1U << 1)
#endif

#ifndef IORING_ASYNC_CANCEL_ANY
#define IORING_ASYNC_CANCEL_ANY (1U << 2)
#endif

struct __kernel_timespec;

/** @brief Raw io_uring_setup(2) syscall wrapper (no libc wrapper exists).
 *  @param entries Ring depth requested.
 *  @param p Out params/offsets filled by the kernel.
 *  @return Ring file descriptor on success, -1 with errno set on failure. */
static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
/* The kernel orders an SQE's publication before its CQE, but ThreadSanitizer
 * cannot see through the io_uring rings, so a connection prepared by one
 * thread and completed on the reactor thread looks like a race. Tell it:
 * every SQ tail publication releases the ring, and the run thread acquires
 * it before dispatching CQEs. No code is generated outside TSan builds. */
#if defined(__SANITIZE_THREAD__)
#define REACTOR_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define REACTOR_TSAN 1
#endif
#endif
#ifdef REACTOR_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#define reactor_tsan_release(ring) __tsan_release((void *)(ring))
#define reactor_tsan_acquire(ring) __tsan_acquire((void *)(ring))
#else
#define reactor_tsan_release(ring) ((void)(ring))
#define reactor_tsan_acquire(ring) ((void)(ring))
#endif

/** @brief Raw io_uring_enter(2) syscall wrapper.
 *  @param ring_fd Ring file descriptor.
 *  @param to_submit Number of queued SQEs to submit.
 *  @param min_complete Minimum completions to wait for (with GETEVENTS).
 *  @param flags IORING_ENTER_* flags.
 *  @param sig Signal mask to apply during the wait, or NULL.
 *  @return 0 or number of completions on success, -1 with errno set on failure. */
static inline int sys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                                     unsigned flags, sigset_t *sig) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig);
}
/** @brief io_uring_enter with an absolute timeout, via IORING_ENTER_EXT_ARG.
 *  @param ring_fd Ring file descriptor.
 *  @param to_submit Number of queued SQEs to submit.
 *  @param min_complete Minimum completions to wait for.
 *  @param flags IORING_ENTER_* flags (EXT_ARG is OR-ed in here).
 *  @param ts Absolute timeout (CLOCK_MONOTONIC) as __kernel_timespec, or NULL.
 *  @return Completions/submit result on success, -1 with errno set (ETIME on
 *          timeout expiry) on failure. */
static inline int sys_io_uring_enter_timeout(int ring_fd, unsigned to_submit, unsigned min_complete,
                                             unsigned flags, const struct __kernel_timespec *ts) {
    struct io_uring_getevents_arg arg = {.ts = (uint64_t)(uintptr_t)ts};
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                        flags | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
}

/** @brief Map one io_uring ring region (SQ ring, CQ ring, or SQE array).
 *
 * No MAP_POPULATE: with one ring per worker thread the pre-faulted pages
 * dominate idle RSS (~400 KiB per reactor) while a worker under real
 * load only ever touches the head of each ring.  On-demand paging keeps
 * RSS proportional to actual concurrency.
 *
 * @param fd Ring file descriptor.
 * @param sz Region size in bytes.
 * @param off Region selector (IORING_OFF_*).
 * @return Mapped pointer, or NULL on failure (caller munmaps and falls back). */
static void *mmap_ring(int fd, size_t sz, off_t off) {
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    return (p == MAP_FAILED) ? NULL : p;
}

/** @brief Byte size of the mmap-ed SQ ring region for the given setup params.
 *  @param p Filled io_uring_params from io_uring_setup.
 *  @return Region size: sq_off.array offset plus the SQ index array. */
static size_t sq_ring_size(struct io_uring_params *p) {
    return p->sq_off.array + p->sq_entries * sizeof(uint32_t);
}

/** @brief Byte size of the mmap-ed CQ ring region for the given setup params.
 *  @param p Filled io_uring_params from io_uring_setup.
 *  @return Region size: cq_off.cqes offset plus the CQE array. */
static size_t cq_ring_size(struct io_uring_params *p) {
    return p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);
}

typedef struct {
    int ring_fd;
    /* Serializes SQ producers: the accept thread (reactor_add) and worker
     * threads (rearm/del) submit concurrently; without this the SQE memcpy
     * and tail advance race and one submission overwrites the other,
     * silently dropping the connection's POLL_ADD. */
    pthread_mutex_t sq_lock;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    uint32_t *sq_head, *sq_tail, *sq_ring_mask, *sq_array;
    uint32_t *cq_head, *cq_tail, *cq_ring_mask;
    uint32_t sq_entries;
    uint32_t cq_entries;
    size_t sq_ring_sz, cq_ring_sz, sqes_sz;
    bool active;

    // epoll fallback
    int epoll_fd;
    bool use_epoll;
} reactor_impl_t;

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>

typedef struct {
    int kq_fd;
} reactor_impl_t;

#else
// Fallback
typedef struct {
} reactor_impl_t;
#endif

typedef struct {
    int fd;
    cwist_reactor_cb_t cb;
    void *ctx;
#ifdef __linux__
    /* CLOCK_MONOTONIC timestamp taken in reactor_add_common right before the
     * POLL_ADD submit/defer decision; approximates when the fd started
     * waiting for readiness. Only set when CWIST_LATENCY_PROBE is enabled;
     * zero means "armed before the probe was enabled" (defensive, see the
     * dispatch loop). */
    uint64_t armed_ns;
#endif
    /* Inline caller payload; ctx always points here. Zeroed on checkout so
     * reuse across events cannot leak stale fields (calloc semantics
     * without the heap).  While the slot sits on the free list, ctx is
     * borrowed as the next-free link. */
    unsigned char payload[CWIST_REACTOR_PAYLOAD_SIZE];
} reactor_event_ctx_t;

/* Slot chunks grow on demand (see struct cwist_reactor below); 512 slots per
 * chunk keeps a mostly-idle worker's footprint small (~40 KiB vs ~320 KiB at
 * 4096) while busy reactors simply chain more chunks. */
#define REACTOR_CHUNK_EVENTS 512

typedef struct reactor_slot_chunk {
    struct reactor_slot_chunk *next;
    /* Slots follow in the same allocation. */
    reactor_event_ctx_t slots[];
} reactor_slot_chunk_t;

struct cwist_reactor {
    reactor_impl_t impl;
    bool running;
    /* Foreign-thread completion stack (Treiber MPSC) plus the self-pipe that
     * wakes a parked run thread: producers push a node and write a byte to
     * wake_wr; the run thread drains the stack on the wake event (and once
     * per loop iteration as a backstop). */
    _Atomic(cwist_reactor_post_t *) post_head;
    int wake_fd;   /* Read side registered with the poller. */
    int wake_wr;   /* Write side (same fd as wake_fd for eventfd). */
    /* Dynamically grown slot chunks: a fixed pool (formerly 4096 slots)
     * capped every reactor at 4096 live connections, which is what shed
     * requests en masse past ~500k concurrent connections.  Chunks are never
     * freed until destroy because slot pointers sit in in-flight CQEs. */
    reactor_slot_chunk_t *chunks;
    reactor_event_ctx_t *free_head;  /* Free list threaded through slots. */
    pthread_mutex_t pool_lock;
    /* Armed one-shot timers, min-heap on deadline_ns.  Run thread only. */
    cwist_reactor_timer_t **timer_heap;
    uint32_t timer_n;
    uint32_t timer_cap;
#ifdef __linux__
    /* Deferred SQE batching: submissions made by the reactor's own run thread
     * while it dispatches a CQE batch (overwhelmingly connection re-arms, one
     * per served request) are queued here and flushed with a single
     * io_uring_enter after the batch, instead of paying one enter syscall per
     * event.  Cross-thread submissions (accept thread -> worker reactor) keep
     * the immediate path so sleeping workers still wake. */
    struct io_uring_sqe deferred_sqes[1024];
    reactor_event_ctx_t *deferred_ctxs[1024];
    uint32_t deferred_n;
    /* SQEs parked in the SQ by queue_deferred, covered by the next wait
     * enter's to_submit.  Only touched by the run thread. */
    uint32_t sq_unsubmitted;
    /* Owned by the run thread: read only after reactor_on_run_thread(), so
     * other threads never touch it. */
    bool dispatching;
    /* True while the run thread dispatches a callback of a round that has
     * run past CWIST_REACTOR_LATE_US (issue #319 experiment). Run thread
     * only; read through cwist_reactor_dispatch_late(). */
    bool dispatch_late;
    uint64_t late_rounds;    /* Rounds that crossed the threshold. */
    uint64_t late_callbacks; /* Callbacks dispatched in late mode. */
    /* RX-uring completion handler for tagged (low-bit set) RECV SQEs;
     * registered by the HTTP layer once per worker reactor. */
    cwist_rx_cb_t rx_cb;
    /* RX-epoll: fds whose armed receive may already have bytes waiting (the
     * last recv filled the buffer, or an edge arrived while unarmed). An
     * edge-triggered registration will not report those again, so the run
     * loop receives them before its next wait. Run thread only. */
    int *rx_ready;
    uint32_t rx_ready_n;
    uint32_t rx_ready_cap;
    /* Per-request latency probe recorder (issue #166). Owner-thread only, so
     * plain counters suffice. Two histograms: time from (re-)arm to dispatch
     * (queue_delay) and callback runtime (svc). */
#define LATENCY_PROBE_BUCKETS 18
    struct {
        uint64_t buckets[LATENCY_PROBE_BUCKETS];
        uint64_t count;
        uint64_t sum_us;
        uint64_t max_us;
        uint64_t over_5ms; /* samples beyond 5000 us */
    } probe[2];
#endif
};

/* The reactor this thread is running in cwist_reactor_run(), or NULL. A
 * thread-local instead of a pthread_t owner field in the reactor: other
 * threads would otherwise read that field while the run thread writes it. */
static __thread cwist_reactor_t *t_run_reactor;

static inline bool reactor_on_run_thread(const cwist_reactor_t *reactor) {
    return t_run_reactor == reactor;
}

/* Microsecond bucket boundaries for the latency probe histograms: 18 buckets
 * spanning [0,10) us up to [1e6,+inf) us. */
#ifdef __linux__
static const uint32_t latency_probe_bounds_us[LATENCY_PROBE_BUCKETS - 1] = {
    10,    25,    50,    100,    250,    500,    1000,    2500,   5000,
    10000, 25000, 50000, 100000, 250000, 500000, 1000000, 2500000};

/** @brief Whether the per-request latency probe is enabled (CWIST_LATENCY_PROBE=1).
 *
 * Cached after the first read like the other env knobs in this file -- the
 * racy recompute is benign (same result every time).
 * @return true when the probe is enabled. */
static bool latency_probe_enabled(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_LATENCY_PROBE");
        v = (s && strcmp(s, "1") == 0) ? 1 : 0;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v == 1;
}

/** @brief Record one latency sample (microseconds) into a probe histogram.
 *
 * Binary-searches the bucket whose upper bound first exceeds the sample,
 * then updates the bucket, count, running sum, max, and over-5ms counters.
 * @param buckets Histogram bucket array (LATENCY_PROBE_BUCKETS entries).
 * @param count In/out total sample count.
 * @param sum_us In/out running sum of samples.
 * @param max_us In/out running maximum.
 * @param over_5ms In/out count of samples beyond 5000 us.
 * @param sample_us The sample value in microseconds. */
static void latency_probe_record(uint64_t *buckets, uint64_t *count, uint64_t *sum_us,
                                 uint64_t *max_us, uint64_t *over_5ms, uint64_t sample_us) {
    int lo = 0, hi = LATENCY_PROBE_BUCKETS - 1;
    while (lo < hi) { /* first bucket whose upper bound exceeds the sample */
        int mid = (lo + hi) / 2;
        if (sample_us < latency_probe_bounds_us[mid])
            hi = mid;
        else
            lo = mid + 1;
    }
    buckets[lo]++;
    (*count)++;
    *sum_us += sample_us;
    if (sample_us > *max_us) *max_us = sample_us;
    if (sample_us > 5000) (*over_5ms)++;
}

enum { LATENCY_PROBE_QUEUE = 0, LATENCY_PROBE_SVC = 1 };

/** @brief Approximate percentile from a histogram: smallest bucket upper
 *         bound whose cumulative count reaches pct (in per-mille) of total.
 *  @param buckets Histogram bucket array.
 *  @param count Total sample count.
 *  @param pct_mille Percentile in per-mille (500 = p50, 999 = p99.9).
 *  @return Bucket upper bound in microseconds, or UINT64_MAX for the
 *          open-ended top bucket. */
static uint64_t latency_probe_percentile(const uint64_t *buckets, uint64_t count,
                                         uint64_t pct_mille) {
    uint64_t target = (count * pct_mille + 999) / 1000;
    uint64_t cumulative = 0;
    for (int i = 0; i < LATENCY_PROBE_BUCKETS; i++) {
        cumulative += buckets[i];
        if (cumulative >= target)
            return i + 1 < LATENCY_PROBE_BUCKETS ? latency_probe_bounds_us[i] : UINT64_MAX;
    }
    return UINT64_MAX;
}

static void latency_probe_dump(const cwist_reactor_t *reactor) {
    if (!latency_probe_enabled()) return;
    if (reactor->late_rounds)
        fprintf(stderr, "[latency-probe] pid=%d late_rounds=%llu late_callbacks=%llu\n",
                (int)getpid(), (unsigned long long)reactor->late_rounds,
                (unsigned long long)reactor->late_callbacks);
    for (int phase = 0; phase < 2; phase++) {
        const uint64_t *b = reactor->probe[phase].buckets;
        uint64_t count = reactor->probe[phase].count;
        if (count == 0) continue;
        const char *name = phase == LATENCY_PROBE_QUEUE ? "queue_delay" : "svc";
        fprintf(stderr, "[latency-probe] pid=%d %-12s count=%llu\n", (int)getpid(), name,
                (unsigned long long)count);
        fprintf(stderr,
                "[latency-probe] pid=%d %-12s p50=%llu p90=%llu p99=%llu p999=%llu max=%llu us\n",
                (int)getpid(), name, (unsigned long long)latency_probe_percentile(b, count, 500),
                (unsigned long long)latency_probe_percentile(b, count, 900),
                (unsigned long long)latency_probe_percentile(b, count, 990),
                (unsigned long long)latency_probe_percentile(b, count, 999),
                (unsigned long long)reactor->probe[phase].max_us);
        fprintf(stderr, "[latency-probe] pid=%d %-12s mean=%llu us over_5ms=%llu\n", (int)getpid(),
                name, (unsigned long long)(reactor->probe[phase].sum_us / count),
                (unsigned long long)reactor->probe[phase].over_5ms);
        for (int i = 0; i < LATENCY_PROBE_BUCKETS; i++) {
            if (!b[i]) continue;
            if (i + 1 < LATENCY_PROBE_BUCKETS)
                fprintf(stderr, "[latency-probe] pid=%d %-12s [%-8u,%-8u) us : %llu\n",
                        (int)getpid(), name, i == 0 ? 0u : latency_probe_bounds_us[i - 1],
                        latency_probe_bounds_us[i], (unsigned long long)b[i]);
            else
                fprintf(stderr, "[latency-probe] pid=%d %-12s [1000000, +inf) us : %llu\n",
                        (int)getpid(), name, (unsigned long long)b[i]);
        }
    }
}

/* --- RX-uring receive path (issue #179) ------------------------------------
 * CWIST_RX_URING: unset = auto (enabled whenever the reactor has a real
 * io_uring ring), "1" = force attempt, "0" = force off (legacy POLL path).
 * Cached after the first read like the other env knobs in this file. */
static bool uring_submit(cwist_reactor_t *reactor, struct io_uring_sqe *out_sqe);

bool cwist_rx_uring_env_enabled(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_RX_URING");
        v = (s && strcmp(s, "0") == 0) ? 0 : 1;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v == 1;
}

/* CWIST_RX_EPOLL: unset = on, "0" = off (epoll reactors keep the one-shot
 * POLL + recv path). Cached like CWIST_RX_URING. */
static bool rx_epoll_env_enabled(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_RX_EPOLL");
        v = (s && strcmp(s, "0") == 0) ? 0 : 1;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v == 1;
}

bool cwist_reactor_rx_supported(const cwist_reactor_t *reactor) {
    if (!reactor) return false;
    if (reactor->impl.use_epoll) return rx_epoll_env_enabled();
    return cwist_rx_uring_env_enabled();
}

/* Per-fd epoll registration state, process-wide (an fd belongs to one
 * reactor at a time). It serves two purposes:
 *
 *  - A hint for the one-shot path: a re-arm of an fd that is already
 *    registered goes straight to EPOLL_CTL_MOD instead of failing an ADD with
 *    EEXIST first, which used to cost two epoll_ctl calls per keep-alive
 *    request.
 *  - The RX-epoll receive path (reactor_rx.h): the fd stays registered
 *    edge-triggered, with no EPOLLONESHOT, and the reactor does the recv() into
 *    the armed buffer itself. A steady keep-alive request then costs a recv
 *    and a send, with no epoll_ctl.
 *
 * state is the only field touched off the run thread (by cwist_reactor_add,
 * _mod and _del, which take an fd out of RX mode); the rest belongs to the
 * run thread of the reactor that armed the fd. Every state value is only a
 * hint for epoll_ctl: a stale one costs one failed call (ENOENT/EEXIST) and
 * the fallback, never a lost registration, except RX, which recv_arm trusts
 * only for the same connection pointer (see cwist_reactor_fd_forget). */
enum { EPFD_UNKNOWN = 0, EPFD_ONESHOT = 1, EPFD_RX = 2 };

typedef struct {
    _Atomic uint8_t state;
    bool armed;      /* A receive is armed: the next readable edge recv()s. */
    bool queued;     /* On the owning reactor's rx_ready list. */
    bool maybe_more; /* Bytes may be waiting that no new edge will report. */
    unsigned len;
    void *buf;
    void *conn;
} epoll_fd_slot_t;

#define EPFD_CHUNK_BITS 10
#define EPFD_CHUNK (1u << EPFD_CHUNK_BITS)
#define EPFD_MAX_FDS (1u << 22)
static _Atomic(epoll_fd_slot_t *) g_epfd_chunks[EPFD_MAX_FDS / EPFD_CHUNK];

/** @brief Slot for @p fd, or NULL when out of range or (unless @p create) not allocated yet. */
static epoll_fd_slot_t *epfd_slot(int fd, bool create) {
    if (fd < 0 || (unsigned)fd >= EPFD_MAX_FDS) return NULL;
    _Atomic(epoll_fd_slot_t *) *chunk = &g_epfd_chunks[(unsigned)fd >> EPFD_CHUNK_BITS];
    epoll_fd_slot_t *c = atomic_load_explicit(chunk, memory_order_acquire);
    if (!c) {
        if (!create) return NULL;
        /* Chunks live as long as the process: reachable from the table and
         * reused by every later fd in their range. */
        epoll_fd_slot_t *fresh = cwist_alloc(EPFD_CHUNK * sizeof(*fresh));
        if (!fresh) return NULL;
        memset(fresh, 0, EPFD_CHUNK * sizeof(*fresh));
        if (atomic_compare_exchange_strong_explicit(chunk, &c, fresh, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            c = fresh;
        } else {
            cwist_free(fresh); /* Another thread installed it first; c holds that one. */
        }
    }
    return &c[(unsigned)fd & (EPFD_CHUNK - 1)];
}

static void epfd_set_state(int fd, uint8_t state) {
    epoll_fd_slot_t *slot = epfd_slot(fd, state != EPFD_UNKNOWN);
    if (slot) atomic_store_explicit(&slot->state, state, memory_order_relaxed);
}

static uint8_t epfd_get_state(int fd) {
    epoll_fd_slot_t *slot = epfd_slot(fd, false);
    return slot ? atomic_load_explicit(&slot->state, memory_order_relaxed) : EPFD_UNKNOWN;
}

void cwist_reactor_fd_forget(int fd) {
    epfd_set_state(fd, EPFD_UNKNOWN);
}

/** @brief Point @p fd's registration on @p epfd at @p ev, trying MOD or ADD first per the hint. */
static bool epoll_register(int epfd, int fd, struct epoll_event *ev) {
    if (epfd_get_state(fd) != EPFD_UNKNOWN) {
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, ev) == 0) return true;
        return errno == ENOENT && epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ev) == 0;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ev) == 0) return true;
    return errno == EEXIST && epoll_ctl(epfd, EPOLL_CTL_MOD, fd, ev) == 0;
}

/** @brief Queue @p fd for a receive before the next wait. Run thread only. */
static bool rx_ready_push(cwist_reactor_t *r, int fd, epoll_fd_slot_t *slot) {
    if (slot->queued) return true;
    if (r->rx_ready_n == r->rx_ready_cap) {
        uint32_t cap = r->rx_ready_cap ? r->rx_ready_cap * 2 : 64;
        int *grown = cwist_alloc(cap * sizeof(*grown));
        if (!grown) return false;
        if (r->rx_ready_n) memcpy(grown, r->rx_ready, r->rx_ready_n * sizeof(*grown));
        cwist_free(r->rx_ready);
        r->rx_ready = grown;
        r->rx_ready_cap = cap;
    }
    r->rx_ready[r->rx_ready_n++] = fd;
    slot->queued = true;
    return true;
}

/** @brief recv() into the armed buffer of @p fd and deliver the result to rx_cb.
 * EAGAIN keeps the receive armed for the next edge. Run thread only. */
static void rx_epoll_recv(cwist_reactor_t *r, int fd, epoll_fd_slot_t *slot) {
    ssize_t n;
    do {
        n = recv(fd, slot->buf, slot->len, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        slot->maybe_more = false;
        return;
    }
    int res = n >= 0 ? (int)n : -errno;
    /* A full buffer may have left bytes behind; a short read drained the
     * socket, so anything newer raises a fresh edge. */
    slot->maybe_more = n > 0 && (size_t)n == slot->len;
    slot->armed = false;
    /* rx_cb may close and free the connection, or re-arm (possibly queueing
     * this fd again); the slot itself stays valid. */
    if (r->rx_cb) r->rx_cb(slot->conn, res);
}

/** @brief Handle one readiness event of an RX-mode fd. Run thread only. */
static void rx_epoll_event(cwist_reactor_t *r, int fd) {
    epoll_fd_slot_t *slot = epfd_slot(fd, false);
    if (!slot || atomic_load_explicit(&slot->state, memory_order_relaxed) != EPFD_RX) return;
    if (!slot->armed) {
        /* The connection is between receives (serving, or parked on a
         * deferred response). Remember the edge for the next arm. */
        slot->maybe_more = true;
        return;
    }
    rx_epoll_recv(r, fd, slot);
}

/** @brief Receive for the fds queued by recv_arm. Arms made by these callbacks
 * queue behind them and run on the next pass. Run thread only. */
static void rx_epoll_drain_ready(cwist_reactor_t *r) {
    uint32_t n = r->rx_ready_n;
    for (uint32_t i = 0; i < n; i++) {
        int fd = r->rx_ready[i];
        epoll_fd_slot_t *slot = epfd_slot(fd, false);
        if (!slot) continue;
        slot->queued = false;
        if (atomic_load_explicit(&slot->state, memory_order_relaxed) != EPFD_RX || !slot->armed) {
            continue;
        }
        rx_epoll_recv(r, fd, slot);
    }
    /* Keep what the callbacks queued during this pass. */
    memmove(r->rx_ready, r->rx_ready + n, (r->rx_ready_n - n) * sizeof(*r->rx_ready));
    r->rx_ready_n -= n;
}

/** @brief RX-epoll arm: see cwist_reactor_recv_arm. */
static bool rx_epoll_arm(cwist_reactor_t *reactor, int fd, void *buf, unsigned len, void *conn) {
    /* Off the run thread the slot would race the run thread's receive; the
     * caller falls back to the one-shot POLL, whose callback arms from the
     * run thread. */
    if (!reactor_on_run_thread(reactor)) return false;
    epoll_fd_slot_t *slot = epfd_slot(fd, true);
    if (!slot) return false;
    bool registered =
        atomic_load_explicit(&slot->state, memory_order_relaxed) == EPFD_RX && slot->conn == conn;
    slot->buf = buf;
    slot->len = len;
    slot->conn = conn;
    if (!registered) {
        /* First arm, or the fd went through the one-shot path since. MOD and
         * ADD both report an fd that is already readable, so no edge is lost
         * across the switch. */
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        ev.data.u64 = ((uint64_t)(unsigned)fd << 1) | 1u;
        if (!epoll_register(reactor->impl.epoll_fd, fd, &ev)) return false;
        atomic_store_explicit(&slot->state, EPFD_RX, memory_order_relaxed);
        slot->maybe_more = false;
        slot->armed = true;
        return true;
    }
    slot->armed = true;
    if (slot->maybe_more && !rx_ready_push(reactor, fd, slot)) {
        slot->armed = false;
        return false;
    }
    return true;
}

void cwist_reactor_set_rx_cb(cwist_reactor_t *reactor, cwist_rx_cb_t cb) {
    if (reactor) reactor->rx_cb = cb;
}

bool cwist_reactor_recv_arm(cwist_reactor_t *reactor, int fd, void *buf, unsigned len, void *conn,
                            uint64_t *armed_ns) {
    if (!reactor || fd < 0 || !buf || len == 0 || !conn) return false;
    if (!cwist_reactor_rx_supported(reactor)) return false;
    if (reactor->impl.use_epoll) {
        if (armed_ns && latency_probe_enabled()) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            *armed_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        }
        return rx_epoll_arm(reactor, fd, buf, len, conn);
    }
    /* Tag-bit discipline: ev_ctx pointers come from cwist_alloc (malloc
     * backed, at least max_align_t aligned), so the low bit is always zero
     * there and one here.  Refuse a misaligned connection pointer rather
     * than aliasing into the ev_ctx namespace. */
    if (((uintptr_t)conn & 1u) != 0) return false;

    struct io_uring_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_RECV;
    sqe.fd = fd;
    sqe.addr = (uint64_t)(uintptr_t)buf;
    sqe.len = len;
    sqe.user_data = (uint64_t)(uintptr_t)conn | 1u;
    if (armed_ns && latency_probe_enabled()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        *armed_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }

    /* Run-thread re-arms defer into the batch flush; everything else
     * submits immediately so remote workers wake.  Data SQEs carry no ev_ctx
     * slot, so deferred_ctxs[] holds NULL for them and flush_deferred knows
     * the connection pointer rides in the tagged user_data. */
    if (reactor_on_run_thread(reactor) && reactor->dispatching &&
        reactor->deferred_n <
            (uint32_t)(sizeof(reactor->deferred_sqes) / sizeof(reactor->deferred_sqes[0]))) {
        uint32_t slot = reactor->deferred_n++;
        reactor->deferred_sqes[slot] = sqe;
        reactor->deferred_ctxs[slot] = NULL;
        return true;
    }
    return uring_submit(reactor, &sqe);
}

void cwist_reactor_probe_record(cwist_reactor_t *reactor, bool queue, uint64_t sample_us) {
    if (!reactor || !latency_probe_enabled()) return;
    int phase = queue ? LATENCY_PROBE_QUEUE : LATENCY_PROBE_SVC;
    latency_probe_record(reactor->probe[phase].buckets, &reactor->probe[phase].count,
                         &reactor->probe[phase].sum_us, &reactor->probe[phase].max_us,
                         &reactor->probe[phase].over_5ms, sample_us);
}
#endif

static reactor_event_ctx_t *alloc_reactor_ctx(cwist_reactor_t *r, int fd, cwist_reactor_cb_t cb,
                                              const void *payload, size_t payload_size) {
    if (!r || payload_size > CWIST_REACTOR_PAYLOAD_SIZE) return NULL;
    pthread_mutex_lock(&r->pool_lock);
    reactor_event_ctx_t *ev_ctx = r->free_head;
    if (ev_ctx) r->free_head = (reactor_event_ctx_t *)ev_ctx->ctx;
    pthread_mutex_unlock(&r->pool_lock);

    if (!ev_ctx) {
        /* Free list empty: grow a chunk. Allocate and link it outside the
         * lock so the ~40 KiB setup does not serialize every other submitter
         * on this reactor; under the lock the chunk is pushed onto the free
         * list wholesale and one slot popped. Chunks are never freed until
         * destroy, so a race with another grower only means two chunks. */
        reactor_slot_chunk_t *chunk =
            cwist_alloc(sizeof(*chunk) + REACTOR_CHUNK_EVENTS * sizeof(reactor_event_ctx_t));
        if (!chunk) return NULL;
        for (uint32_t i = 0; i + 1 < REACTOR_CHUNK_EVENTS; i++) {
            chunk->slots[i].ctx = &chunk->slots[i + 1];
        }
        chunk->slots[REACTOR_CHUNK_EVENTS - 1].ctx = NULL;

        pthread_mutex_lock(&r->pool_lock);
        chunk->next = r->chunks;
        r->chunks = chunk;
        chunk->slots[REACTOR_CHUNK_EVENTS - 1].ctx = r->free_head;
        r->free_head = &chunk->slots[0];
        ev_ctx = r->free_head;
        r->free_head = (reactor_event_ctx_t *)ev_ctx->ctx;
        pthread_mutex_unlock(&r->pool_lock);
    }
    if (!ev_ctx) return NULL;

    memset(ev_ctx, 0, sizeof(*ev_ctx));
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ev_ctx->payload;
    if (payload && payload_size > 0) {
        memcpy(ev_ctx->payload, payload, payload_size);
    }
    return ev_ctx;
}

static void free_reactor_ctx(cwist_reactor_t *r, reactor_event_ctx_t *ev_ctx) {
    if (!r || !ev_ctx) return;
    pthread_mutex_lock(&r->pool_lock);
    ev_ctx->ctx = r->free_head;
    r->free_head = ev_ctx;
    pthread_mutex_unlock(&r->pool_lock);
}

bool cwist_reactor_post(cwist_reactor_t *r, cwist_reactor_post_t *node) {
    if (!r || !node || !node->cb) return false;
    cwist_reactor_post_t *head = atomic_load_explicit(&r->post_head, memory_order_relaxed);
    do {
        node->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&r->post_head, &head, node,
                                                    memory_order_release, memory_order_relaxed));
    /* Wake only on the first post of a pending batch. A non-empty stack
     * already has a wake pending; another write would be redundant. */
    if (head == NULL && r->wake_wr >= 0) {
        uint64_t one = 1;
        ssize_t ign = write(r->wake_wr, &one, sizeof(one));
        (void)ign; /* EAGAIN means the run thread is already awake. */
    }
    return true;
}

/* Pop the whole MPSC stack and run the callbacks oldest-first.  Only ever
 * called by the reactor's run thread (or destroy, after it has stopped). */
static void reactor_drain_posts(cwist_reactor_t *r) {
    cwist_reactor_post_t *list =
        atomic_exchange_explicit(&r->post_head, NULL, memory_order_acquire);
    cwist_reactor_post_t *rev = NULL;
    while (list) {
        cwist_reactor_post_t *next = list->next;
        list->next = rev;
        rev = list;
        list = next;
    }
    while (rev) {
        cwist_reactor_post_t *next = rev->next;
        rev->cb(rev->ctx);
        rev = next;
    }
}

/* run flag: cleared by cwist_reactor_stop() from any thread. */
static bool reactor_running(cwist_reactor_t *r) {
    return __atomic_load_n(&r->running, __ATOMIC_ACQUIRE) && atomic_load(&g_cwist_running);
}

/* ---- one-shot timers (min-heap, run thread only) ---- */

/* Upper bound on any poll wait, timers or not: the shutdown flags are only
 * re-checked between waits (see cwist_reactor_run). */
#define REACTOR_IDLE_WAIT_NS 100000000ull

static uint64_t reactor_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void timer_heap_place(cwist_reactor_t *r, uint32_t i, cwist_reactor_timer_t *t) {
    r->timer_heap[i] = t;
    t->heap_slot = i + 1;
}

static void timer_heap_up(cwist_reactor_t *r, uint32_t i) {
    cwist_reactor_timer_t *t = r->timer_heap[i];
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (r->timer_heap[parent]->deadline_ns <= t->deadline_ns) break;
        timer_heap_place(r, i, r->timer_heap[parent]);
        i = parent;
    }
    timer_heap_place(r, i, t);
}

static void timer_heap_down(cwist_reactor_t *r, uint32_t i) {
    cwist_reactor_timer_t *t = r->timer_heap[i];
    for (;;) {
        uint32_t child = 2 * i + 1;
        if (child >= r->timer_n) break;
        if (child + 1 < r->timer_n &&
            r->timer_heap[child + 1]->deadline_ns < r->timer_heap[child]->deadline_ns)
            child++;
        if (t->deadline_ns <= r->timer_heap[child]->deadline_ns) break;
        timer_heap_place(r, i, r->timer_heap[child]);
        i = child;
    }
    timer_heap_place(r, i, t);
}

static void timer_heap_remove(cwist_reactor_t *r, cwist_reactor_timer_t *t) {
    uint32_t i = t->heap_slot - 1;
    t->heap_slot = 0;
    cwist_reactor_timer_t *last = r->timer_heap[--r->timer_n];
    if (i == r->timer_n) return;
    timer_heap_place(r, i, last);
    if (i > 0 && r->timer_heap[(i - 1) / 2]->deadline_ns > last->deadline_ns)
        timer_heap_up(r, i);
    else
        timer_heap_down(r, i);
}

void cwist_reactor_timer_init(cwist_reactor_timer_t *timer, void (*cb)(void *ctx), void *ctx) {
    if (!timer) return;
    timer->deadline_ns = 0;
    timer->heap_slot = 0;
    timer->cb = cb;
    timer->ctx = ctx;
}

bool cwist_reactor_timer_arm(cwist_reactor_t *r, cwist_reactor_timer_t *timer, uint64_t delay_us) {
    if (!r || !timer || !timer->cb) return false;
    if (timer->heap_slot) timer_heap_remove(r, timer);
    if (r->timer_n == r->timer_cap) {
        uint32_t cap = r->timer_cap ? r->timer_cap * 2 : 64;
        cwist_reactor_timer_t **heap = cwist_alloc(cap * sizeof(*heap));
        if (!heap) return false;
        if (r->timer_n) memcpy(heap, r->timer_heap, r->timer_n * sizeof(*heap));
        cwist_free(r->timer_heap);
        r->timer_heap = heap;
        r->timer_cap = cap;
    }
    timer->deadline_ns = reactor_now_ns() + delay_us * 1000ull;
    r->timer_n++;
    timer_heap_place(r, r->timer_n - 1, timer);
    timer_heap_up(r, r->timer_n - 1);
    return true;
}

void cwist_reactor_timer_cancel(cwist_reactor_t *r, cwist_reactor_timer_t *timer) {
    if (!r || !timer || !timer->heap_slot) return;
    timer_heap_remove(r, timer);
}

bool cwist_reactor_timer_armed(const cwist_reactor_timer_t *timer) {
    return timer && timer->heap_slot != 0;
}

/* Fire every timer whose deadline has passed.  At most the timers armed on
 * entry run, so a callback that re-arms itself with a zero delay waits for
 * the next round instead of spinning here. */
static void reactor_run_timers(cwist_reactor_t *r) {
    if (r->timer_n == 0) return;
    uint64_t now = reactor_now_ns();
    uint32_t budget = r->timer_n;
    while (budget-- > 0 && r->timer_n > 0 && r->timer_heap[0]->deadline_ns <= now) {
        cwist_reactor_timer_t *t = r->timer_heap[0];
        timer_heap_remove(r, t);
        t->cb(t->ctx);
    }
}

/* Poll wait for this round: the time to the earliest timer, capped at the
 * idle wait. */
static uint64_t reactor_wait_ns(const cwist_reactor_t *r) {
    if (r->timer_n == 0) return REACTOR_IDLE_WAIT_NS;
    uint64_t now = reactor_now_ns();
    uint64_t deadline = r->timer_heap[0]->deadline_ns;
    if (deadline <= now) return 0;
    uint64_t wait = deadline - now;
    return wait < REACTOR_IDLE_WAIT_NS ? wait : REACTOR_IDLE_WAIT_NS;
}

static void reactor_wake_cb(int fd, void *ctx) {
    cwist_reactor_t *r = *(cwist_reactor_t *const *)ctx;
    uint64_t buf[8];
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
    reactor_drain_posts(r);
    /* One-shot slots are recycled after firing: re-arm for the next post. */
    if (!cwist_reactor_add(r, fd, reactor_wake_cb, &r, sizeof(r))) {
        int wr = r->wake_wr;
        r->wake_fd = -1;
        r->wake_wr = -1;
        close(fd);
        if (wr >= 0 && wr != fd) close(wr);
    }
}

cwist_reactor_t *cwist_reactor_create(void) {
    cwist_reactor_t *r = cwist_alloc(sizeof(cwist_reactor_t));
    if (!r) return NULL;
    memset(r, 0, sizeof(cwist_reactor_t));
    __atomic_store_n(&r->running, false, __ATOMIC_RELEASE);
    pthread_mutex_init(&r->pool_lock, NULL);
#ifdef __linux__
    pthread_mutex_init(&r->impl.sq_lock, NULL);
#endif

#ifdef __linux__
    r->impl.use_epoll = false;
    /* CWIST_REACTOR_BACKEND=epoll forces the epoll path for A/B measurement;
     * any other value (or unset) keeps io_uring as the default. */
    {
        const char *backend = getenv("CWIST_REACTOR_BACKEND");
        if (backend && strcmp(backend, "epoll") == 0) r->impl.use_epoll = true;
    }
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = r->impl.use_epoll ? -1 : sys_io_uring_setup(4096, &p);
    if (fd >= 0) {
        r->impl.ring_fd = fd;
        r->impl.sq_ring_sz = sq_ring_size(&p);
        r->impl.cq_ring_sz = cq_ring_size(&p);
        r->impl.sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);

        void *sq_ptr = mmap_ring(fd, r->impl.sq_ring_sz, IORING_OFF_SQ_RING);
        void *cq_ptr = mmap_ring(fd, r->impl.cq_ring_sz, IORING_OFF_CQ_RING);
        void *sqes_ptr = mmap_ring(fd, r->impl.sqes_sz, IORING_OFF_SQES);

        if (sq_ptr && cq_ptr && sqes_ptr) {
            r->impl.sq_head = (uint32_t *)((char *)sq_ptr + p.sq_off.head);
            r->impl.sq_tail = (uint32_t *)((char *)sq_ptr + p.sq_off.tail);
            r->impl.sq_ring_mask = (uint32_t *)((char *)sq_ptr + p.sq_off.ring_mask);
            r->impl.sq_array = (uint32_t *)((char *)sq_ptr + p.sq_off.array);
            r->impl.sqes = sqes_ptr;
            r->impl.sq_entries = p.sq_entries;

            r->impl.cq_head = (uint32_t *)((char *)cq_ptr + p.cq_off.head);
            r->impl.cq_tail = (uint32_t *)((char *)cq_ptr + p.cq_off.tail);
            r->impl.cq_ring_mask = (uint32_t *)((char *)cq_ptr + p.cq_off.ring_mask);
            r->impl.cqes = (struct io_uring_cqe *)((char *)cq_ptr + p.cq_off.cqes);
            r->impl.cq_entries = p.cq_entries;
            r->impl.active = true;

            /* Identity SQE mapping is fixed for the ring's lifetime; fill it
             * once here instead of rewriting sq_array on every submission. */
            for (uint32_t i = 0; i < p.sq_entries; i++) r->impl.sq_array[i] = i;
        } else {
            if (sq_ptr) munmap(sq_ptr, r->impl.sq_ring_sz);
            if (cq_ptr) munmap(cq_ptr, r->impl.cq_ring_sz);
            if (sqes_ptr) munmap(sqes_ptr, r->impl.sqes_sz);
            close(fd);
            r->impl.use_epoll = true;
        }
    } else {
        r->impl.use_epoll = true;
    }

    if (r->impl.use_epoll) {
        r->impl.epoll_fd = epoll_create1(0);
        if (r->impl.epoll_fd < 0) {
            pthread_mutex_destroy(&r->impl.sq_lock);
            pthread_mutex_destroy(&r->pool_lock);
            cwist_free(r);
            return NULL;
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    r->impl.kq_fd = kqueue();
    if (r->impl.kq_fd < 0) {
        pthread_mutex_destroy(&r->pool_lock);
        cwist_free(r);
        return NULL;
    }
#endif
    atomic_init(&r->post_head, NULL);
#ifdef __linux__
    r->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    r->wake_wr = r->wake_fd;
#else
    r->wake_fd = -1;
    r->wake_wr = -1;
    {
        int pfds[2];
        if (pipe(pfds) == 0) {
            for (int i = 0; i < 2; i++) {
                int fl = fcntl(pfds[i], F_GETFL, 0);
                if (fl >= 0) fcntl(pfds[i], F_SETFL, fl | O_NONBLOCK);
            }
            r->wake_fd = pfds[0];
            r->wake_wr = pfds[1];
        }
    }
#endif
    /* Poll the wake fd on every backend. IORING_REGISTER_EVENTFD notifies
     * an external eventfd when CQEs arrive; writes to it do not create CQEs
     * or interrupt io_uring_enter. The one-shot poll is re-armed by
     * reactor_wake_cb, including when posts arrive during a dispatch. */
    if (r->wake_fd >= 0 && !cwist_reactor_add(r, r->wake_fd, reactor_wake_cb, &r, sizeof(r))) {
        /* Wake best-effort: the run loop still drains the stack each round. */
        close(r->wake_fd);
        if (r->wake_wr != r->wake_fd) close(r->wake_wr);
        r->wake_fd = -1;
        r->wake_wr = -1;
    }
    return r;
}

void cwist_reactor_destroy(cwist_reactor_t *reactor) {
    if (!reactor) return;
#ifdef __linux__
    latency_probe_dump(reactor);
#endif
    /* Run any completions posted after the run thread parked for good. */
    reactor_drain_posts(reactor);
    if (reactor->wake_fd >= 0) close(reactor->wake_fd);
    if (reactor->wake_wr >= 0 && reactor->wake_wr != reactor->wake_fd) close(reactor->wake_wr);
#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* Teardown absorbed from io_uring_backend.c: unmap all three rings. */
        if (reactor->impl.sqes) munmap(reactor->impl.sqes, reactor->impl.sqes_sz);
        if (reactor->impl.cqes) {
            munmap((char *)reactor->impl.cqes -
                       (reactor->impl.cq_ring_sz -
                        reactor->impl.cq_entries * sizeof(struct io_uring_cqe)),
                   reactor->impl.cq_ring_sz);
        }
        if (reactor->impl.sq_array) {
            munmap((char *)reactor->impl.sq_array -
                       (reactor->impl.sq_ring_sz - reactor->impl.sq_entries * sizeof(uint32_t)),
                   reactor->impl.sq_ring_sz);
        }
        close(reactor->impl.ring_fd);
    } else {
        close(reactor->impl.epoll_fd);
        cwist_free(reactor->rx_ready);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    close(reactor->impl.kq_fd);
#endif
    pthread_mutex_destroy(&reactor->pool_lock);
#ifdef __linux__
    pthread_mutex_destroy(&reactor->impl.sq_lock);
#endif
    cwist_free(reactor->timer_heap);
    reactor_slot_chunk_t *chunk = reactor->chunks;
    while (chunk) {
        reactor_slot_chunk_t *next = chunk->next;
        cwist_free(chunk);
        chunk = next;
    }
    cwist_free(reactor);
}

#ifdef __linux__
/* Submit one SQE immediately: the reactor never batches or defers submission,
 * so a woken worker always sees the event on its next wait. */
static bool uring_submit(cwist_reactor_t *reactor, struct io_uring_sqe *out_sqe) {
    bool ok = false;
    pthread_mutex_lock(&reactor->impl.sq_lock);
    uint32_t tail = *reactor->impl.sq_tail;
    uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
    if (tail - head < reactor->impl.sq_entries) {
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memcpy(sqe, out_sqe, sizeof(*sqe));
        reactor_tsan_release(&reactor->impl);
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            if (cwist_async_debug_enabled()) {
                static _Atomic long dbg_enter_fail;
                long n = atomic_fetch_add(&dbg_enter_fail, 1) + 1;
                if (n <= 5 || n % 10000 == 0)
                    fprintf(stderr,
                            "[reactor] uring enter failed fd=%d errno=%d(%s) sq=%u/%u total=%ld\n",
                            out_sqe->fd, errno, strerror(errno), tail - head,
                            reactor->impl.sq_entries, n);
            }
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
        } else {
            ok = true;
        }
    } else if (cwist_async_debug_enabled()) {
        static _Atomic long dbg_sq_full;
        long n = atomic_fetch_add(&dbg_sq_full, 1) + 1;
        if (n <= 5 || n % 10000 == 0)
            fprintf(stderr, "[reactor] SQ full fd=%d depth=%u/%u total=%ld\n", out_sqe->fd,
                    tail - head, reactor->impl.sq_entries, n);
    }
    pthread_mutex_unlock(&reactor->impl.sq_lock);
    return ok;
}
#endif

#ifdef __linux__
/* Flush the deferred SQE queue with a single io_uring_enter for the whole
 * batch.  On batch failure (SQ momentarily full from concurrent cross-thread
 * submissions) fall back to per-SQE submits with a short retry; a final
 * failure closes the connection and recycles its slot, matching every
 * caller's add-failure path. */
static bool uring_submit_batch(cwist_reactor_t *reactor, struct io_uring_sqe *batch, uint32_t n) {
    bool ok = false;
    pthread_mutex_lock(&reactor->impl.sq_lock);
    uint32_t tail = *reactor->impl.sq_tail;
    uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
    if (tail - head + n <= reactor->impl.sq_entries) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t index = (tail + i) & *reactor->impl.sq_ring_mask;
            memcpy(&reactor->impl.sqes[index], &batch[i], sizeof(batch[i]));
        }
        reactor_tsan_release(&reactor->impl);
        __atomic_store_n(reactor->impl.sq_tail, tail + n, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, n, 0, 0, NULL) >= 0) {
            ok = true;
        } else {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&reactor->impl.sq_lock);
    return ok;
}

static void flush_deferred(cwist_reactor_t *reactor) {
    uint32_t n = reactor->deferred_n;
    reactor->deferred_n = 0;
    if (n == 0) return;
    if (uring_submit_batch(reactor, reactor->deferred_sqes, n)) return;
    for (uint32_t i = 0; i < n; i++) {
        struct io_uring_sqe *sqe = &reactor->deferred_sqes[i];
        reactor_event_ctx_t *ev_ctx = reactor->deferred_ctxs[i];
        int attempt;
        for (attempt = 0; attempt < 100; attempt++) {
            if (uring_submit(reactor, sqe)) break;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
            nanosleep(&ts, NULL); /* SQ drains without our help; wait it out */
        }
        if (attempt == 100) {
            if (cwist_async_debug_enabled()) {
                fprintf(stderr, "[reactor] deferred submit failed fd=%d; closing\n", (int)sqe->fd);
            }
            if (ev_ctx) {
                close((int)sqe->fd);
                free_reactor_ctx(reactor, ev_ctx);
            } else if (reactor->rx_cb) {
                /* Tagged RECV SQE: the connection owns the fd and shell, so
                 * report the cancellation and let its handler clean up. */
                reactor->rx_cb((void *)(sqe->user_data & ~(uint64_t)1u), -ECANCELED);
            }
        }
    }
}

/* Copy the deferred SQEs into the SQ without an io_uring_enter: the run
 * loop's next wait enter carries to_submit, so a dispatch round costs one
 * enter total instead of wait-enter + flush-enter.  On SQ contention fall
 * back to the immediate flush so re-arms never stall. */
static void queue_deferred(cwist_reactor_t *reactor) {
    uint32_t n = reactor->deferred_n;
    if (n == 0) return;
    pthread_mutex_lock(&reactor->impl.sq_lock);
    uint32_t tail = *reactor->impl.sq_tail;
    uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
    if (tail - head + n <= reactor->impl.sq_entries) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t index = (tail + i) & *reactor->impl.sq_ring_mask;
            memcpy(&reactor->impl.sqes[index], &reactor->deferred_sqes[i],
                   sizeof(reactor->deferred_sqes[i]));
        }
        reactor_tsan_release(&reactor->impl);
        __atomic_store_n(reactor->impl.sq_tail, tail + n, __ATOMIC_RELEASE);
        reactor->deferred_n = 0;
        reactor->sq_unsubmitted += n;
    }
    pthread_mutex_unlock(&reactor->impl.sq_lock);
    if (reactor->deferred_n) flush_deferred(reactor);
}
#endif

static bool reactor_add_common(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                               const void *payload, size_t payload_size, bool for_write) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, payload, payload_size);
    if (!ev_ctx) return false;

#ifdef __linux__
    if (latency_probe_enabled()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ev_ctx->armed_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    if (!reactor->impl.use_epoll) {
        /* One-shot POLL_ADD: multishot (IORING_POLL_ADD_MULTI) was rejected
         * because the slot is recycled after firing, so a persistent poll
         * would re-dispatch into a recycled slot. */
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_POLL_ADD;
        sqe.fd = fd;
        sqe.poll_events = for_write ? POLLOUT : POLLIN;
        sqe.user_data = (uint64_t)ev_ctx;
        /* Run-thread re-arms defer to the batch flush; everything else
         * submits immediately so remote workers wake from their wait. */
        if (reactor_on_run_thread(reactor) && reactor->dispatching &&
            reactor->deferred_n <
                (uint32_t)(sizeof(reactor->deferred_sqes) / sizeof(reactor->deferred_sqes[0]))) {
            uint32_t slot = reactor->deferred_n++;
            reactor->deferred_sqes[slot] = sqe;
            reactor->deferred_ctxs[slot] = ev_ctx;
            return true;
        }
        if (uring_submit(reactor, &sqe)) {
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    } else {
        struct epoll_event ev;
        ev.events = (for_write ? EPOLLOUT : EPOLLIN) | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_register(reactor->impl.epoll_fd, fd, &ev)) {
            /* Also takes the fd out of RX mode: the one-shot registration
             * replaced the edge-triggered one. */
            epfd_set_state(fd, EPFD_ONESHOT);
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, for_write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0,
           0, ev_ctx);
    if (kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0) {
        return true;
    }
    free_reactor_ctx(reactor, ev_ctx);
    return false;
#endif
    free_reactor_ctx(reactor, ev_ctx);
    return false;
}

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, const void *payload,
                       size_t payload_size) {
    return reactor_add_common(reactor, fd, cb, payload, payload_size, false);
}

bool cwist_reactor_add_out(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                           const void *payload, size_t payload_size) {
    return reactor_add_common(reactor, fd, cb, payload, payload_size, true);
}

bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, const void *payload,
                       size_t payload_size) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, payload, payload_size);
    if (!ev_ctx) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_POLL_ADD;
        sqe.fd = fd;
        sqe.poll_events = POLLIN;
        sqe.user_data = (uint64_t)ev_ctx;
        if (uring_submit(reactor, &sqe)) {
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            epfd_set_state(fd, EPFD_ONESHOT);
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    if (kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0) {
        return true;
    }
    free_reactor_ctx(reactor, ev_ctx);
    return false;
#endif
    free_reactor_ctx(reactor, ev_ctx);
    return false;
}

bool cwist_reactor_del(cwist_reactor_t *reactor, int fd) {
    if (!reactor || fd < 0) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_ASYNC_CANCEL;
        sqe.addr = (unsigned long)fd;
        sqe.cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        sqe.user_data = 0;
        return uring_submit(reactor, &sqe);
    } else {
        struct epoll_event ev;
        epfd_set_state(fd, EPFD_UNKNOWN);
        return epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0;
#else
    return false;
#endif
}

#ifdef __linux__
/* CWIST_REACTOR_DRAIN_CHUNK: cooperative-queuing knob (see the comment at
 * its call site). Values > 0 bound how many connection callbacks run before
 * foreign-thread posts are drained. 0 disables the mid-batch drain (legacy
 * behavior). Unset or invalid defaults to 64 so a foreign-thread completion
 * does not queue behind a full CQE batch by default. Cached after the first
 * read like the other env knobs in this file -- the racy recompute is benign
 * (same result every time). */
static uint32_t reactor_drain_chunk(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_REACTOR_DRAIN_CHUNK");
        if (s) {
            long parsed = strtol(s, NULL, 10);
            v = (parsed >= 0 && parsed < INT_MAX) ? (int)parsed : 64;
        } else {
            v = 64;
        }
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return (uint32_t)v;
}

/* CQ grace: how long a reactor loop peeks its completion ring (pure
 * userspace, no syscall) before committing to the kernel wait. Completions
 * that land while a dispatch batch is being processed -- the common shape
 * under keep-alive load, where the peer's next request often arrives within
 * microseconds of the response -- would otherwise force a fresh wake each;
 * the grace folds them into the current one. 0 disables the spin.
 * CWIST_REACTOR_CQ_GRACE_NS overrides the 20 us default. */
static uint64_t reactor_cq_grace_ns(void) {
    static _Atomic long cached = -1;
    long v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_REACTOR_CQ_GRACE_NS");
        if (s && *s) {
            char *end = NULL;
            long parsed = strtol(s, &end, 10);
            v = (end != s && parsed >= 0 && parsed < 1000000000L) ? parsed : 20000;
        } else {
            v = 20000;
        }
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return (uint64_t)v;
}

/* Round budget: an io_uring dispatch round drains every CQE in the ring, and
 * a busy round can hold hundreds of connections' callbacks. Callbacks re-arm
 * their connections into the deferred SQE array, which only reaches the
 * kernel when the round ends -- so a request that arrived right after its
 * connection was re-armed waits out the whole batch, which is where the
 * arm-to-dispatch tail comes from (issue #166). The budget bounds that wait
 * without breaking the round: past it, parked re-arms are flushed to the
 * kernel at the next check instead of at round end (one extra enter per
 * check, only while a round is long). The round still runs to completion,
 * so the stop-at-round-boundary contract is unchanged. The check rides its
 * own cadence (every 64 callbacks), independent of CWIST_REACTOR_DRAIN_CHUNK.
 * 0 disables the bound. CWIST_REACTOR_ROUND_BUDGET_US overrides the 2000 us
 * default. */
static uint64_t reactor_round_budget_us(void) {
    static _Atomic long cached = -1;
    long v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_REACTOR_ROUND_BUDGET_US");
        if (s && *s) {
            char *end = NULL;
            long parsed = strtol(s, &end, 10);
            v = (end != s && parsed >= 0 && parsed < 3600000000L) ? parsed : 2000;
        } else {
            v = 2000;
        }
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return (uint64_t)v;
}

#if defined(__x86_64__) || defined(__i386__)
#define reactor_cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define reactor_cpu_relax() __asm__ __volatile__("yield")
#else
#define reactor_cpu_relax() ((void)0)
#endif
#endif

#ifdef __linux__
/* Late-round threshold (issue #319 experiment). Once a dispatch round has
 * run this long since its completions were reaped, every remaining callback
 * of the round is dispatched in late mode: its re-arm (and any parked so
 * far) is submitted right after it instead of at round end, foreign-thread
 * posts are drained after each callback, and the HTTP layer serves the
 * connection's buffered pipeline without yielding. The connections behind a
 * slow round have already waited the threshold; late mode stops them from
 * waiting for the rest of it too. CWIST_REACTOR_LATE_US overrides the
 * 3000 us default; 0 disables. */
static uint64_t reactor_late_ns(void) {
    static _Atomic long cached = -1;
    long v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_REACTOR_LATE_US");
        if (s && *s) {
            char *end = NULL;
            long parsed = strtol(s, &end, 10);
            v = (end != s && *end == '\0' && parsed >= 0 && parsed < 3600000000L) ? parsed : 3000;
        } else {
            v = 3000;
        }
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return (uint64_t)v * 1000ull;
}

bool cwist_reactor_dispatch_late(void) {
    const cwist_reactor_t *r = t_run_reactor;
    return r && r->dispatching && r->dispatch_late;
}
#endif

void cwist_reactor_run(cwist_reactor_t *reactor) {
    if (!reactor) return;
    __atomic_store_n(&reactor->running, true, __ATOMIC_RELEASE);
    t_run_reactor = reactor;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        const uint64_t cq_grace_ns = reactor_cq_grace_ns();
        const uint64_t late_ns = reactor_late_ns();
        while (reactor_running(reactor)) {
            reactor_drain_posts(reactor);
            reactor_run_timers(reactor);
            /* Submit the re-arms queued by the previous dispatch batch under
             * the SQ lock, then wait in a separate call.  The submit enter
             * MUST hold the lock: uring_submit/uring_submit_batch roll the
             * SQ tail back when their own enter fails, and a concurrent
             * unlocked enter here could consume that SQE first, leaving
             * sq_tail behind sq_head.  tail < head reads as a permanently
             * full SQ to every later submit (tail - head underflows past
             * sq_entries), so the reactor goes deaf: no re-arm, no accept,
             * no completions - the server stops answering while every thread
             * looks idle.  With all SQ consumers under the lock, a failed
             * enter means the SQE was definitely not consumed and the
             * rollback is exact.
             *
             * The wait is bounded (like the epoll path's 100 ms poll)
             * because the shutdown handler runs with SA_RESTART: an
             * unbounded enter would be restarted after SIGTERM and hang an
             * idle reactor forever.  It passes to_submit=0, so it consumes
             * nothing and needs no lock. */
            /* CQ grace: completions that land while the previous batch is
             * dispatching would otherwise force a fresh wake. After the
             * locked submit enter below, peek the CQ ring from userspace for
             * a bounded window; completions caught in time are dispatched in
             * the current round and the wait enter is skipped. Re-arms are
             * safe either way: the submit enter above already flushed them.
             * The deadline bounds the spin; on expiry (or when disabled) the
             * code falls through to the bounded wait below. */
            bool cq_pending = false;
            if (cq_grace_ns > 0) {
                uint32_t ph =
                    __atomic_load_n(reactor->impl.cq_head, __ATOMIC_ACQUIRE);
                if (ph == *reactor->impl.cq_tail) {
                    struct timespec gs;
                    clock_gettime(CLOCK_MONOTONIC, &gs);
                    uint64_t g_end = (uint64_t)gs.tv_sec * 1000000000ull +
                                     (uint64_t)gs.tv_nsec + cq_grace_ns;
                    uint32_t spins = 0;
                    while (ph == *reactor->impl.cq_tail) {
                        reactor_cpu_relax();
                        if (((++spins) & 31u) == 0) {
                            clock_gettime(CLOCK_MONOTONIC, &gs);
                            uint64_t now = (uint64_t)gs.tv_sec * 1000000000ull +
                                           (uint64_t)gs.tv_nsec;
                            if (now >= g_end) break;
                        }
                        ph = __atomic_load_n(reactor->impl.cq_head, __ATOMIC_ACQUIRE);
                    }
                    cq_pending = (ph != *reactor->impl.cq_tail);
                }
            }

            /* Bounded by the idle wait, shortened to the next timer. */
            const uint64_t wait_ns = reactor_wait_ns(reactor);
            const struct __kernel_timespec idle_ts = {
                .tv_sec = (long long)(wait_ns / 1000000000ull),
                .tv_nsec = (long long)(wait_ns % 1000000000ull)};
            uint32_t to_submit = reactor->sq_unsubmitted;
            reactor->sq_unsubmitted = 0;
            if (to_submit > 0) {
                pthread_mutex_lock(&reactor->impl.sq_lock);
                int sret = sys_io_uring_enter(reactor->impl.ring_fd, to_submit, 0, 0, NULL);
                pthread_mutex_unlock(&reactor->impl.sq_lock);
                if (sret < 0) {
                    /* Submission state is unknown on EINTR; anything else
                     * failed before consuming.  Either way the SQEs are
                     * still in the SQ (the kernel caps resubmission at what
                     * is actually there), so restore the count and let the
                     * next round retry. */
                    reactor->sq_unsubmitted += to_submit;
                }
            }
            if (!cq_pending) {
                int ret = sys_io_uring_enter_timeout(reactor->impl.ring_fd, 0, 1,
                                                     IORING_ENTER_GETEVENTS, &idle_ts);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    if (errno == ETIME) continue;
                    break;
                }
            }
            uint32_t head = __atomic_load_n(reactor->impl.cq_head, __ATOMIC_ACQUIRE);
            uint32_t tail = *reactor->impl.cq_tail;
            if (head == tail) {
                continue;
            }
            reactor_tsan_acquire(&reactor->impl);
            reactor->dispatching = true;
            const uint64_t round_budget_us = reactor_round_budget_us();
            uint64_t round_start_ns = 0;
            if (round_budget_us || late_ns) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                round_start_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            }
            /* Cooperative queuing: a busy round can carry hundreds of ready
             * connections in one CQE batch (the ring is 4096 deep). Foreign-
             * thread completions (cwist_async_defer, background jobs) queue
             * onto post_head via cwist_reactor_post() and used to wait for
             * reactor_drain_posts() at the *top* of the next round -- i.e.
             * behind this entire batch, even if the post arrived while we
             * were only a few callbacks in. CWIST_REACTOR_DRAIN_CHUNK bounds
             * how many connection callbacks run before posts are drained, so
             * a foreign-thread completion's own tail latency stops scaling
             * with how many *other* connections happened to be ready in the
             * same wake. The default (64) applies the bound out of the box;
             * 0 restores the legacy single-drain-at-end behavior. */
            uint32_t drain_chunk = reactor_drain_chunk();
            uint32_t since_drain = 0;
            uint32_t since_budget_check = 0;
            bool round_late = false;
            while (head != tail) {
                if (late_ns && !round_late) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    uint64_t now_ns =
                        (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                    if (now_ns - round_start_ns >= late_ns) {
                        round_late = true;
                        reactor->dispatch_late = true;
                        reactor->late_rounds++;
                    }
                }
                struct io_uring_cqe *cqe = &reactor->impl.cqes[head & *reactor->impl.cq_ring_mask];
                uint64_t user_data = cqe->user_data;
                if (user_data & 1u) {
                    /* Tagged data-path SQE (RX-uring RECV): the connection
                     * pointer rides in the remaining bits.  Dispatch on any
                     * res, including negative ones: -EAGAIN is the POLL
                     * fallback signal and 0 is EOF.  Latency probe samples
                     * are recorded by the handler itself (it owns the arm
                     * timestamp), not by the ev_ctx wrapper below. */
                    if (reactor->rx_cb) {
                        reactor->rx_cb((void *)(user_data & ~(uint64_t)1u), cqe->res);
                    }
                } else {
                    reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)user_data;
                    if (ev_ctx) {
                        if (cqe->res >= 0) {
                            if (latency_probe_enabled()) {
                                struct timespec ts;
                                clock_gettime(CLOCK_MONOTONIC, &ts);
                                uint64_t t0 =
                                    (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                                /* armed_ns == 0 means the slot was armed before
                                 * the probe was enabled; fold it into the first
                                 * bucket rather than producing a garbage delay. */
                                uint64_t delay_ns = ev_ctx->armed_ns ? t0 - ev_ctx->armed_ns : 0;
                                ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                                clock_gettime(CLOCK_MONOTONIC, &ts);
                                uint64_t t1 =
                                    (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                                latency_probe_record(reactor->probe[LATENCY_PROBE_QUEUE].buckets,
                                                     &reactor->probe[LATENCY_PROBE_QUEUE].count,
                                                     &reactor->probe[LATENCY_PROBE_QUEUE].sum_us,
                                                     &reactor->probe[LATENCY_PROBE_QUEUE].max_us,
                                                     &reactor->probe[LATENCY_PROBE_QUEUE].over_5ms,
                                                     delay_ns / 1000);
                                latency_probe_record(reactor->probe[LATENCY_PROBE_SVC].buckets,
                                                     &reactor->probe[LATENCY_PROBE_SVC].count,
                                                     &reactor->probe[LATENCY_PROBE_SVC].sum_us,
                                                     &reactor->probe[LATENCY_PROBE_SVC].max_us,
                                                     &reactor->probe[LATENCY_PROBE_SVC].over_5ms,
                                                     (t1 - t0) / 1000);
                            } else {
                                ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                            }
                        }
                        free_reactor_ctx(reactor, ev_ctx);
                    }
                }
                head++;
                if (round_late) {
                    /* Late mode: nothing this connection queued waits for
                     * the rest of the round. */
                    reactor->late_callbacks++;
                    __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
                    flush_deferred(reactor);
                    reactor_drain_posts(reactor);
                    continue;
                }
                if (drain_chunk && ++since_drain >= drain_chunk && head != tail) {
                    since_drain = 0;
                    __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
                    reactor_drain_posts(reactor);
                }
                if (round_budget_us && head != tail &&
                    (++since_budget_check & 63u) == 0) {
                    /* The budget check rides its own cadence (every 64
                     * callbacks), independent of CWIST_REACTOR_DRAIN_CHUNK,
                     * so disabling the post drain does not disable the
                     * round bound. */
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    uint64_t now_ns =
                        (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                    if (now_ns - round_start_ns >= round_budget_us * 1000ull) {
                        /* Long round: flush the re-arms parked so far instead
                         * of holding them to round end. The batch itself
                         * still runs to completion. */
                        flush_deferred(reactor);
                    }
                }
            }
            reactor->dispatching = false;
            reactor->dispatch_late = false;
            __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
            queue_deferred(reactor);
        }
    } else {
        struct epoll_event events[1024];
        while (reactor_running(reactor)) {
            reactor_drain_posts(reactor);
            reactor_run_timers(reactor);
            /* Round the wait up so a timer is never polled for early. */
            int wait_ms = (int)((reactor_wait_ns(reactor) + 999999ull) / 1000000ull);
            /* Queued RX receives must not wait behind a blocking poll. */
            if (reactor->rx_ready_n > 0) wait_ms = 0;
            int n = epoll_wait(reactor->impl.epoll_fd, events, 1024, wait_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; i++) {
                /* Low bit set: an RX-mode fd (fd << 1 | 1); ev_ctx slots are
                 * at least 8-byte aligned and never carry it. */
                if (events[i].data.u64 & 1u) {
                    rx_epoll_event(reactor, (int)(events[i].data.u64 >> 1));
                    continue;
                }
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].data.ptr;
                if (ev_ctx) {
                    ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    free_reactor_ctx(reactor, ev_ctx);
                }
            }
            if (reactor->rx_ready_n > 0) rx_epoll_drain_ready(reactor);
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent events[1024];
    /* Bounded like the epoll and io_uring waits: a shutdown requested
     * without a signal (cwist_shutdown_request() from another thread) only
     * clears g_cwist_running and closes the listen socket, which wakes
     * no kevent, so the flag must be re-checked periodically. */
    while (reactor_running(reactor)) {
        reactor_drain_posts(reactor);
        reactor_run_timers(reactor);
        const uint64_t wait_ns = reactor_wait_ns(reactor);
        const struct timespec idle_ts = {.tv_sec = (time_t)(wait_ns / 1000000000ull),
                                         .tv_nsec = (long)(wait_ns % 1000000000ull)};
        int n = kevent(reactor->impl.kq_fd, NULL, 0, events, 1024, &idle_ts);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].udata;
            if (ev_ctx) {
                ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                free_reactor_ctx(reactor, ev_ctx);
            }
        }
    }
#endif
    t_run_reactor = NULL;
}

void cwist_reactor_stop(cwist_reactor_t *reactor) {
    if (!reactor) return;
    /* Called from foreign threads (shutdown paths, embedders). */
    __atomic_store_n(&reactor->running, false, __ATOMIC_RELEASE);
    /* A run thread parked in io_uring_enter(GETEVENTS) on an idle SQPOLL
     * ring sleeps until a CQE arrives: the kernel ignores the enter
     * timeout for SQPOLL rings, so with no pending SQEs the wait never
     * returns and the pthread_join() in the pool destroy path hangs the
     * process (observed as a worker child surviving SIGTERM until SIGKILL,
     * wedging the supervisor's shutdown waitpid). Nudge the registered
     * wake fd so the run thread re-checks its shutdown flags. */
    if (reactor->wake_wr >= 0) {
        uint64_t one = 1;
        ssize_t ign = write(reactor->wake_wr, &one, sizeof(one));
        (void)ign; /* EAGAIN: a wake is already pending, which is fine. */
    }
}
