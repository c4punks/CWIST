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
    /* No MAP_POPULATE: with one ring per worker thread the pre-faulted pages
     * dominate idle RSS (~400 KiB per reactor) while a worker under real
     * load only ever touches the head of each ring.  On-demand paging keeps
     * RSS proportional to actual concurrency. */
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
    size_t sq_ring_sz, cq_ring_sz;
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
    /* Inline caller payload; ctx always points here. Zeroed on checkout so
     * reuse across events cannot leak stale fields (calloc semantics
     * without the heap). */
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
    /* Dynamically grown slot chunks: a fixed pool (formerly 4096 slots)
     * capped every reactor at 4096 live connections, which is what shed
     * requests en masse past ~500k concurrent connections.  Chunks are never
     * freed until destroy because slot pointers sit in in-flight CQEs. */
    reactor_slot_chunk_t *chunks;
    reactor_event_ctx_t *free_head;  /* Free list threaded through slots. */
    pthread_mutex_t pool_lock;
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
    pthread_t owner;
    bool dispatching;
#endif
};

static reactor_event_ctx_t *alloc_reactor_ctx(cwist_reactor_t *r, int fd, cwist_reactor_cb_t cb,
                                              const void *payload, size_t payload_size) {
    if (!r || payload_size > CWIST_REACTOR_PAYLOAD_SIZE) return NULL;
    pthread_mutex_lock(&r->pool_lock);
    if (r->free_top == 0) {
        pthread_mutex_unlock(&r->pool_lock);
        return NULL;
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
                        (int)getpid(), name, i == 0 ? 0 : latency_probe_bounds_us[i - 1],
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

bool cwist_reactor_rx_supported(const cwist_reactor_t *reactor) {
    return reactor && !reactor->impl.use_epoll && cwist_rx_uring_env_enabled();
}

void cwist_reactor_set_rx_cb(cwist_reactor_t *reactor, cwist_rx_cb_t cb) {
    if (reactor) reactor->rx_cb = cb;
}

bool cwist_reactor_recv_arm(cwist_reactor_t *reactor, int fd, void *buf, unsigned len, void *conn,
                            uint64_t *armed_ns) {
    if (!reactor || fd < 0 || !buf || len == 0 || !conn) return false;
    if (!cwist_reactor_rx_supported(reactor)) return false;
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
    if (reactor->dispatching && pthread_equal(pthread_self(), reactor->owner) &&
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

    reactor_event_ctx_t *ev_ctx = &r->event_pool[idx];
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
        r->impl.sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
        r->impl.cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
        
        void *sq_ptr = mmap(NULL, r->impl.sq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        void *cq_ptr = mmap(NULL, r->impl.cq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        void *sqes_ptr = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
        
        if (sq_ptr != MAP_FAILED && cq_ptr != MAP_FAILED && sqes_ptr != MAP_FAILED) {
            r->impl.sq_head         = (uint32_t *)((char *)sq_ptr + p.sq_off.head);
            r->impl.sq_tail         = (uint32_t *)((char *)sq_ptr + p.sq_off.tail);
            r->impl.sq_ring_mask    = (uint32_t *)((char *)sq_ptr + p.sq_off.ring_mask);
            r->impl.sq_array        = (uint32_t *)((char *)sq_ptr + p.sq_off.array);
            r->impl.sqes            = sqes_ptr;
            r->impl.sq_entries      = p.sq_entries;
            
            r->impl.cq_head         = (uint32_t *)((char *)cq_ptr + p.cq_off.head);
            r->impl.cq_tail         = (uint32_t *)((char *)cq_ptr + p.cq_off.tail);
            r->impl.cq_ring_mask    = (uint32_t *)((char *)cq_ptr + p.cq_off.ring_mask);
            r->impl.cqes            = (struct io_uring_cqe *)((char *)cq_ptr + p.cq_off.cqes);
            r->impl.cq_entries      = p.cq_entries;
            r->impl.active = true;

            /* Identity SQE mapping is fixed for the ring's lifetime; fill it
             * once here instead of rewriting sq_array on every submission. */
            for (uint32_t i = 0; i < p.sq_entries; i++) r->impl.sq_array[i] = i;
        } else {
            if (sq_ptr != MAP_FAILED) munmap(sq_ptr, r->impl.sq_ring_sz);
            if (cq_ptr != MAP_FAILED) munmap(cq_ptr, r->impl.cq_ring_sz);
            if (sqes_ptr != MAP_FAILED) munmap(sqes_ptr, p.sq_entries * sizeof(struct io_uring_sqe));
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
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
        } else {
            ok = true;
        }
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
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
            nanosleep(&ts, NULL); /* SQ drains without our help; wait it out */
        }
        if (attempt == 100) {
            if (getenv("CWIST_ASYNC_DEBUG")) {
                fprintf(stderr, "[reactor] deferred submit failed fd=%d; closing\n", (int)sqe->fd);
            }
            close((int)sqe->fd);
            free_reactor_ctx(reactor, ev_ctx);
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
        __atomic_store_n(reactor->impl.sq_tail, tail + n, __ATOMIC_RELEASE);
        reactor->deferred_n = 0;
        reactor->sq_unsubmitted += n;
    }
    pthread_mutex_unlock(&reactor->impl.sq_lock);
    if (reactor->deferred_n) flush_deferred(reactor);
}
#endif

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, payload, payload_size);
    if (!ev_ctx) return false;
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* One-shot POLL_ADD: multishot (IORING_POLL_ADD_MULTI) was rejected
         * because the slot is recycled after firing, so a persistent poll
         * would re-dispatch into a recycled slot. */
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_POLL_ADD;
        sqe.fd = fd;
        sqe.poll_events = POLLIN;
        sqe.user_data = (uint64_t)ev_ctx;
        if (uring_submit(reactor, &sqe)) {
            return true;
        }
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fd;
        sqe->poll_events = POLLIN;
        sqe->user_data = (uint64_t)ev_ctx;
        
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            cwist_free(ev_ctx);
            return false;
        }
        return true;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            return true;
        }
        if (errno == EEXIST) {
            if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
                return true;
            }
        }
        cwist_free(ev_ctx);
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

bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, payload, payload_size);
    if (!ev_ctx) return false;
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        uint32_t tail = *reactor->impl.sq_tail;
        uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= reactor->impl.sq_entries) {
            cwist_free(ev_ctx);
            return false;
        }
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fd;
        sqe->poll_events = POLLIN;
        sqe->user_data = (uint64_t)ev_ctx;
        
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            cwist_free(ev_ctx);
            return false;
        }
        return true;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            return true;
        }
        cwist_free(ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    return kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0;
#endif
    return false;
}

bool cwist_reactor_del(cwist_reactor_t *reactor, int fd) {
    if (!reactor || fd < 0) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* Cancel any pending poll request for this fd.  The original
         * request's CQE (with -ECANCELED) will free the ev_ctx. */
        uint32_t tail = *reactor->impl.sq_tail;
        uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= reactor->impl.sq_entries) return false;
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_ASYNC_CANCEL;
        sqe->addr = (unsigned long)fd;
        sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        sqe->user_data = 0;
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            return false;
        }
        return true;
    } else {
        struct epoll_event ev;
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

void cwist_reactor_run(cwist_reactor_t *reactor) {
    if (!reactor) return;
    reactor->running = true;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        while (reactor->running && atomic_load(&g_cwist_running)) {
            /* One enter per round: submit the re-arms queued by the previous
             * dispatch batch and wait for the next event in the same call. */
            uint32_t to_submit = reactor->sq_unsubmitted;
            int ret = sys_io_uring_enter(reactor->impl.ring_fd, to_submit, 1, IORING_ENTER_GETEVENTS, NULL);
            if (ret < 0) {
                if (errno == EINTR) continue; /* sq_unsubmitted kept, retried */
                break;
            }
            reactor->sq_unsubmitted = 0;
            uint32_t head = __atomic_load_n(reactor->impl.cq_head, __ATOMIC_ACQUIRE);
            uint32_t tail = *reactor->impl.cq_tail;
            if (head == tail) {
                continue;
            }
            while (head != tail) {
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
            __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
            queue_deferred(reactor);
        }
    } else {
        struct epoll_event events[1024];
        while (reactor->running && atomic_load(&g_cwist_running)) {
            int n = epoll_wait(reactor->impl.epoll_fd, events, 1024, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            /* Dispatch listening/accept events first, then active sockets in LRU order */
            for (int i = 0; i < n; i++) {
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].data.ptr;
                if (ev_ctx) {
                    ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    free_reactor_ctx(reactor, ev_ctx);
                }
            }
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent events[1024];
    while (reactor->running && atomic_load(&g_cwist_running)) {
        int n = kevent(reactor->impl.kq_fd, NULL, 0, events, 1024, NULL);
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
