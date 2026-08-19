/**
 * @file reactor.c
 * @brief Readiness multiplexer (io_uring / epoll / kqueue) driving CWIST's
 * synchronous callback model.
 *
 * Design note — why readiness notification + synchronous completion is kept:
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

/* Ring setup helpers absorbed from the retired io_uring_backend.c. */
static void *mmap_ring(int fd, size_t sz, off_t off) {
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, off);
    return (p == MAP_FAILED) ? NULL : p;
}

static size_t sq_ring_size(struct io_uring_params *p) {
    return p->sq_off.array + p->sq_entries * sizeof(uint32_t);
}

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
    reactor_event_ctx_t event_pool[MAX_REACTOR_EVENTS];
    /* Free-stack over event_pool (pattern absorbed from io_uring_backend.c's
     * stream table): O(1) slot acquire/release instead of a linear scan. */
    uint32_t free_stack[MAX_REACTOR_EVENTS];
    uint32_t free_top;
    pthread_mutex_t pool_lock;
};

static reactor_event_ctx_t *alloc_reactor_ctx(cwist_reactor_t *r, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->pool_lock);
    if (r->free_top == 0) {
        pthread_mutex_unlock(&r->pool_lock);
        return NULL;
    }
    uint32_t idx = r->free_stack[--r->free_top];
    pthread_mutex_unlock(&r->pool_lock);

    reactor_event_ctx_t *ev_ctx = &r->event_pool[idx];
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;
    return ev_ctx;
}

static void free_reactor_ctx(cwist_reactor_t *r, reactor_event_ctx_t *ev_ctx) {
    if (!r || !ev_ctx) return;
    uint32_t idx = (uint32_t)(ev_ctx - r->event_pool);
    pthread_mutex_lock(&r->pool_lock);
    r->free_stack[r->free_top++] = idx;
    pthread_mutex_unlock(&r->pool_lock);
}

cwist_reactor_t *cwist_reactor_create(void) {
    cwist_reactor_t *r = cwist_alloc(sizeof(cwist_reactor_t));
    if (!r) return NULL;
    memset(r, 0, sizeof(cwist_reactor_t));
    r->running = false;
    pthread_mutex_init(&r->pool_lock, NULL);
    for (uint32_t i = 0; i < MAX_REACTOR_EVENTS; i++) {
        r->free_stack[i] = (MAX_REACTOR_EVENTS - 1) - i;
    }
    r->free_top = MAX_REACTOR_EVENTS;

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
            munmap((char *)reactor->impl.cqes - (reactor->impl.cq_ring_sz -
                   reactor->impl.cq_entries * sizeof(struct io_uring_cqe)),
                   reactor->impl.cq_ring_sz);
        }
        if (reactor->impl.sq_array) {
            munmap((char *)reactor->impl.sq_array - (reactor->impl.sq_ring_sz -
                   reactor->impl.sq_entries * sizeof(uint32_t)),
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
    cwist_free(reactor);
}

#ifdef __linux__
/* Submit one SQE immediately: the reactor never batches or defers submission,
 * so a woken worker always sees the event on its next wait. */
static bool uring_submit(cwist_reactor_t *reactor, struct io_uring_sqe *out_sqe) {
    uint32_t tail = *reactor->impl.sq_tail;
    uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= reactor->impl.sq_entries) return false;
    uint32_t index = tail & *reactor->impl.sq_ring_mask;
    struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
    memcpy(sqe, out_sqe, sizeof(*sqe));
    __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
    if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
        __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}
#endif

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, ctx);
    if (!ev_ctx) return false;

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

static bool reactor_add_common(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                               const void *payload, size_t payload_size, bool for_write) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, payload, payload_size);
    if (!ev_ctx) return false;
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;

#ifdef __linux__
    if (latency_probe_enabled()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ev_ctx->armed_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    if (!reactor->impl.use_epoll) {
        /* One-shot POLL_ADD: multishot (IORING_POLL_ADD_MULTI) was rejected
         * because the callback owns ctx and frees it after firing, so a
         * persistent poll would re-dispatch into a recycled slot. */
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
        ev.events = (for_write ? EPOLLOUT : EPOLLIN) | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            return true;
        }
        if (errno == EEXIST) {
            if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
                return true;
            }
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, for_write ? EVFILT_WRITE : EVFILT_READ,
           EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    if (kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0) {
        return true;
    }
    free_reactor_ctx(reactor, ev_ctx);
    return false;
#endif
    free_reactor_ctx(reactor, ev_ctx);
    return false;
}

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size) {
    return reactor_add_common(reactor, fd, cb, payload, payload_size, false);
}

bool cwist_reactor_add_out(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                           const void *payload, size_t payload_size) {
    return reactor_add_common(reactor, fd, cb, payload, payload_size, true);
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
/* CWIST_REACTOR_DRAIN_CHUNK: opt-in cooperative-queuing knob (see the
 * comment at its call site). 0 (default, or unset/invalid) preserves the
 * legacy behavior of draining a whole CQE batch before servicing foreign-
 * thread posts. Cached after the first read like the other env knobs in
 * this file -- the racy recompute is benign (same result every time). */
static uint32_t reactor_drain_chunk(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char *s = getenv("CWIST_REACTOR_DRAIN_CHUNK");
        long parsed = s ? strtol(s, NULL, 10) : 0;
        v = (parsed > 0 && parsed < INT_MAX) ? (int)parsed : 0;
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

#if defined(__x86_64__) || defined(__i386__)
#define reactor_cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define reactor_cpu_relax() __asm__ __volatile__("yield")
#else
#define reactor_cpu_relax() ((void)0)
#endif
#endif

void cwist_reactor_run(cwist_reactor_t *reactor) {
    if (!reactor) return;
    __atomic_store_n(&reactor->running, true, __ATOMIC_RELEASE);

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        reactor->owner = pthread_self();
        const uint64_t cq_grace_ns = reactor_cq_grace_ns();
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
            reactor->dispatching = true;
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
             * same wake. 0 (default) keeps the legacy single-drain-at-end
             * behavior byte-for-byte. */
            uint32_t drain_chunk = reactor_drain_chunk();
            uint32_t since_drain = 0;
            while (head != tail) {
                struct io_uring_cqe *cqe = &reactor->impl.cqes[head & *reactor->impl.cq_ring_mask];
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)cqe->user_data;
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
                head++;
                if (drain_chunk && ++since_drain >= drain_chunk && head != tail) {
                    since_drain = 0;
                    __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
                    reactor_drain_posts(reactor);
                }
            }
            reactor->dispatching = false;
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
            int n = epoll_wait(reactor->impl.epoll_fd, events, 1024, wait_ms);
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
