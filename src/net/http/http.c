#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && \
    !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#ifndef __wasi__
#include <signal.h>
#endif
#include <time.h>

#include <sys/types.h>
#include <unistd.h>
#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)
#include <sys/wait.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#ifndef __wasi__
#include <sys/resource.h>
#endif
#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/sendfile.h>
#endif
#if defined(__linux__)
#include <sys/sendfile.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/uio.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

/**
 * @brief Return the number of CPUs available to this process.
 *
 * Honors the process CPU affinity mask where the platform supports it
 * (sched_getaffinity on Linux); falls back to sysconf(3)/sysctl and
 * ultimately 1 when the count cannot be determined.
 * @return Number of usable CPUs, at least 1.
 */
long get_cpu_cores(void) {
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (long)sysinfo.dwNumberOfProcessors;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        int count = CPU_COUNT(&cpuset);
        if (count > 0) return (long)count;
    }
#if defined(_SC_NPROCESSORS_ONLN)
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) return nproc;
#endif
    return 1;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    int mib[2];
    int nproc = 0;
    size_t len = sizeof(nproc);
    mib[0] = CTL_HW;
#if defined(HW_NCPUONLINE)
    mib[1] = HW_NCPUONLINE;
#else
    mib[1] = HW_NCPU;
#endif
    if (sysctl(mib, 2, &nproc, &len, NULL, 0) == 0) {
        return (long)nproc;
    }
    return 4;
#elif defined(_SC_NPROCESSORS_ONLN)
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) return nproc;
    return 1;
#else
    return 1;
#endif
}

static unsigned int g_http_pool_core_limit = 0;

/**
 * @brief Fix the HTTP pool worker count to a specific value.
 * @param limit Fixed thread count; 0 restores automatic sizing.
 */
void cwist_http_pool_limit_core(unsigned int limit) {
    g_http_pool_core_limit = limit;
}

/**
 * @brief Compute the HTTP worker/event-loop thread count.
 *
 * Resolution order: a value fixed with cwist_http_pool_limit_core(), then the
 * CWIST_WORKER_THREADS environment override, then CPU-core based sizing
 * modified by CWIST_WORKERS (worker-process count) and CWIST_C1M_MODE.
 * C1M mode divides the core budget across worker processes (one event loop
 * per CPU); classic mode multiplies it to cover blocking keep-alive handlers.
 * @return Computed thread count, clamped to the mode-specific range.
 */
long get_optimal_thread_count(void) {
    if (g_http_pool_core_limit > 0) {
        return (long)g_http_pool_core_limit;
    }
    const char *env = getenv("CWIST_WORKER_THREADS");
    if (env && env[0]) {
        char *end = NULL;
        long override = strtol(env, &end, 10);
        if (end != env && *end == '\0' && override > 0) return override;
    }

    long cores = get_cpu_cores();
    if (cores < 1) cores = 1;

    long workers = cores;
    const char *w_env = getenv("CWIST_WORKERS");
    if (w_env && w_env[0]) {
        if (strcmp(w_env, "auto") != 0) {
            char *end = NULL;
            long parsed = strtol(w_env, &end, 10);
            if (end != w_env && *end == '\0' && parsed > 0) workers = parsed;
        }
    }

    const char *c1m = getenv("CWIST_C1M_MODE");
    bool is_c1m = !c1m || (c1m[0] != '0' && strcmp(c1m, "false") != 0);

    if (is_c1m) {
        /* In event-driven C1M mode each reactor thread multiplexes I/O
         * asynchronously. Target one event loop per CPU across the whole
         * deployment: extra loops on the same CPU add no parallelism, only
         * scheduler queueing, and the queueing lands on the tail. The CI
         * benchmark matrix tracks this configuration on every run.
         *
         * `cores` is this process's own CPU budget, not the machine's:
         * get_cpu_cores() reads sched_getaffinity(), and with workers > 1
         * every worker has already been pinned to a single CPU by
         * cwist_app_pin_worker() before the pool is built. So in the
         * default pinned deployment cores is 1 here and the division
         * below yields 0, clamped to 1 -- one loop per process. Unpinned
         * (e.g. the CI benchmark, which deliberately sees the whole
         * runner), cores is the machine count and the worker processes
         * share it, so the division spreads the loops evenly across them.
         *
         * A higher floor would protect synchronous handoffs (h2c) and
         * blocking handlers, but extra loops on a shared CPU cannot run
         * concurrently there either: a blocking handler stalls its own
         * worker's connections until it yields, while the other workers
         * (other CPUs, shared listener) keep serving. */
        long count = workers > 0 ? cores / workers : cores;
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        return count;
    }

    if (workers == 1) {
        /* Keep-alive handlers park on their connection, so the pool must
         * cover many more concurrent connections than there are cores.
         * Blocked threads are nearly free (futex sleep); undersizing the
         * pool caps throughput at threads x per-conn rate. */
        long count = cores * 8;
        if (count < 32) count = 32;
        if (count > 256) count = 256;
        return count;
    }

    /* Dynamic thread downscaling: distribute thread budget proportionally across forked worker
     * processes */
    long threads_per_worker = (cores * 8) / workers;
    if (threads_per_worker < 8) threads_per_worker = 8;
    if (threads_per_worker > 64) threads_per_worker = 64;
    return threads_per_worker;
}

#define HTTP_TASKS_PER_THREAD 32768

typedef struct {
    pthread_t thread;
    cwist_reactor_t *reactor;
    uint32_t worker_id;
} http_thread_worker_t;

static size_t g_rr_index = 0;
static long g_http_thread_count = 0;
static http_thread_worker_t *g_workers = NULL;
static _Atomic uint32_t *g_worker_loads = NULL;

static _Atomic long g_http_inflight = 0;
static _Atomic bool g_http_pool_stopping = false;
static _Atomic long g_http_continuation_shed = 0;

/* Generation of the C1M worker reactors. cwist_http_pool_destroy() bumps it
 * before it stops and frees them, so a deferred completion that still holds
 * a reactor from an earlier generation (cwist_async outlives the server)
 * closes its connection instead of posting into freed memory. Posters count
 * themselves in g_reactor_posters around the check and the post; destroy
 * waits for that count to drain after the bump, so every post that saw the
 * old generation lands before the reactors' final drain. Both sides use
 * sequentially consistent atomics, so one of them always sees the other. */
static _Atomic uint64_t g_reactor_gen = 0;
static _Atomic long g_reactor_posters = 0;

/**
 * @brief Return the current reactor generation counter.
 *
 * Deferred completions capture the generation so they can detect pool
 * teardown; see g_reactor_gen for the protocol.
 */
uint64_t cwist_http_reactor_generation(void) {
    return atomic_load(&g_reactor_gen);
}

/**
 * @brief Post a reactor node only while the captured generation is current.
 *
 * Counts the caller in g_reactor_posters around the generation check and the
 * post so cwist_http_pool_destroy() cannot free the reactor in between.
 * @param reactor Reactor to post into.
 * @param gen Generation the completion was created under.
 * @param node Post node to enqueue.
 * @return true when the node was posted; false when the generation moved or
 *         the post failed (the caller must close instead).
 */
bool cwist_http_reactor_post_live(cwist_reactor_t *reactor, uint64_t gen,
                                  cwist_reactor_post_t *node) {
    atomic_fetch_add(&g_reactor_posters, 1);
    bool posted = atomic_load(&g_reactor_gen) == gen && cwist_reactor_post(reactor, node);
    atomic_fetch_sub(&g_reactor_posters, 1);
    return posted;
}

/** Continuations dropped because the reactor post queue was full; each one
 * closed its connection. Exported for metrics/observability. */
long cwist_http_continuation_shed_count(void) {
    return atomic_load_explicit(&g_http_continuation_shed, memory_order_relaxed);
}

/** Live C1M connections (accepted, not yet closed). The shutdown drain in
 * cwist_app_listen_ex polls this to exit early once nothing is left. */
long cwist_http_inflight_count(void) {
    return atomic_load_explicit(&g_http_inflight, memory_order_relaxed);
}
#define CWIST_HTTP_INFLIGHT_PER_THREAD 32
#define CWIST_HTTP_INFLIGHT_FD_RESERVE 4096

/**
 * @brief Compute the per-process cap on concurrently served connections.
 *
 * Base is the thread count times CWIST_HTTP_INFLIGHT_PER_THREAD; on POSIX
 * systems the RLIMIT_NOFILE budget minus a reserve raises it when higher.
 * @return Inflight connection limit (always >= the thread-count floor).
 */
static long cwist_http_inflight_limit(void) {
    long base = g_http_thread_count > 0 ? g_http_thread_count : get_optimal_thread_count();
    long floor = base * CWIST_HTTP_INFLIGHT_PER_THREAD;
#ifndef __wasi__
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return floor;
    long budget = (rl.rlim_cur == RLIM_INFINITY)
                      ? (1024L * 1024L)
                      : (long)rl.rlim_cur - CWIST_HTTP_INFLIGHT_FD_RESERVE;
    return budget > floor ? budget : floor;
#else
    /* WASI preview1 has no rlimit; the thread-count floor is enough. */
    return floor;
#endif
}

static const char CWIST_HTTP_503[] =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static _Thread_local http_thread_worker_t *t_current_worker = NULL;

/**
 * @brief Reactor worker main loop for the C1M pool.
 * @param arg http_thread_worker_t for this thread.
 * @return NULL when the reactor stops.
 */
static void *http_pool_worker(void *arg) {
    http_thread_worker_t *w = (http_thread_worker_t *)arg;
    t_current_worker = w;
    ttak_net_lattice_set_worker_id(w->worker_id);

    cwist_reactor_run(w->reactor);
    return NULL;
}

/* Dynamic Thread Pool for Classic Mode
 * Combines pre-allocated idle worker threads with fast job queue and
 * on-demand scaling to eliminate starvation on compute/keepalive workloads
 * while avoiding per-connection pthread_create overhead. */
#define CWIST_POOL_STACK_SIZE (256 * 1024)

typedef struct http_pool_task {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
    struct http_pool_task *next;
} http_pool_task_t;

typedef struct {
    http_pool_task_t *head;
    http_pool_task_t *tail;
    atomic_long pending_tasks;
    atomic_long active_workers;
    atomic_long idle_workers;
    atomic_long max_workers;
    atomic_bool running;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} http_dynamic_pool_t;

static http_dynamic_pool_t g_dyn_pool;

/* RX-uring RECV completion entry for the async connection path; defined
 * with the async helpers below.  Registered as the reactor's rx_cb in
 * cwist_http_pool_init. */
static void http_rx_recv_cb(void *conn_ptr, int res);

/**
 * @brief Main loop of a classic-mode dynamic pool worker thread.
 *
 * Pops tasks from the shared queue, runs their handler, and exits after an
 * idle timeout when the pool is above its base size (scale-down).
 * @return NULL when the pool stops or the worker scales down.
 */
static void *http_dynamic_worker_thread(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_dyn_pool.running, memory_order_acquire)) {
        http_pool_task_t *task = NULL;

        pthread_mutex_lock(&g_dyn_pool.lock);
        while (atomic_load_explicit(&g_dyn_pool.running, memory_order_acquire) &&
               !g_dyn_pool.head) {
            /* Scale-down idle timeout: CWIST_POOL_IDLE_TIMEOUT_MS overrides
             * the 2s default; 0 parks surplus threads forever (use with
             * CWIST_POOL_PREWARM to eliminate spawn churn entirely). */
            static _Atomic long idle_timeout_ms = -1;
            long ms = atomic_load_explicit(&idle_timeout_ms, memory_order_relaxed);
            if (ms < 0) {
                const char *env = getenv("CWIST_POOL_IDLE_TIMEOUT_MS");
                if (env && *env) {
                    char *end = NULL;
                    long v = strtol(env, &end, 10);
                    ms = (end != env && *end == '\0') ? v : 2000;
                } else {
                    ms = 2000;
                }
                if (ms < 0) ms = 2000;
                atomic_store_explicit(&idle_timeout_ms, ms, memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&g_dyn_pool.idle_workers, 1, memory_order_relaxed);
            int rc;
            if (ms == 0) {
                rc = pthread_cond_wait(&g_dyn_pool.cond, &g_dyn_pool.lock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += ms / 1000;
                ts.tv_nsec += (ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                rc = pthread_cond_timedwait(&g_dyn_pool.cond, &g_dyn_pool.lock, &ts);
            }
            atomic_fetch_sub_explicit(&g_dyn_pool.idle_workers, 1, memory_order_relaxed);
            if (rc == ETIMEDOUT && !g_dyn_pool.head) {
                /* Scale down if idle and above base worker threshold */
                long current =
                    atomic_load_explicit(&g_dyn_pool.active_workers, memory_order_relaxed);
                if (current > g_http_thread_count) {
                    atomic_fetch_sub_explicit(&g_dyn_pool.active_workers, 1, memory_order_relaxed);
                    pthread_mutex_unlock(&g_dyn_pool.lock);
                    return NULL;
                }
            }
        }

        if (!atomic_load_explicit(&g_dyn_pool.running, memory_order_acquire)) {
            pthread_mutex_unlock(&g_dyn_pool.lock);
            break;
        }

        task = g_dyn_pool.head;
        if (task) {
            g_dyn_pool.head = task->next;
            if (!g_dyn_pool.head) g_dyn_pool.tail = NULL;
            atomic_fetch_sub_explicit(&g_dyn_pool.pending_tasks, 1, memory_order_release);
        }
        pthread_mutex_unlock(&g_dyn_pool.lock);

        if (task) {
            int fd = task->client_fd;
            void (*handler)(int, void *) = task->handler_func;
            void *ctx = task->ctx;
            cwist_free(task);

            ttak_net_lattice_set_worker_id((uint32_t)(uintptr_t)pthread_self());
            if (handler) handler(fd, ctx);
            atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        }
    }
    return NULL;
}

/**
 * @brief Spawn one detached dynamic-pool worker thread.
 *
 * Uses a reduced stack size (CWIST_POOL_STACK_SIZE), retrying with the
 * default stack when the pthread implementation rejects it.
 * @return true when the worker was created; false otherwise (the
 *         active-worker count is rolled back).
 */
static bool http_spawn_worker(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Cap worker stack reservation: the default 8MB dwarfs the worker's real
     * worst case (16KB read_buf + 8KB header_buf + 64KB file-stream fallback
     * buffer plus call-chain slack).  Note glibc rejects sizes smaller than
     * the process' static TLS footprint (allocatestack.c), which is why the
     * HTTP/3 batch scratch buffers must stay out of TLS. */
    if (pthread_attr_setstacksize(&attr, CWIST_POOL_STACK_SIZE) != 0) {
        CWIST_LOG_WARN("[http] pthread_attr_setstacksize(%d) failed; using default stack",
                       CWIST_POOL_STACK_SIZE);
    }
    pthread_t tid;
    atomic_fetch_add_explicit(&g_dyn_pool.active_workers, 1, memory_order_relaxed);
    int rc = pthread_create(&tid, &attr, http_dynamic_worker_thread, NULL);
    if (rc != 0) {
        /* Retry with the default stack rather than losing the worker. */
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&tid, &attr, http_dynamic_worker_thread, NULL);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_fetch_sub_explicit(&g_dyn_pool.active_workers, 1, memory_order_relaxed);
        return false;
    }
    return true;
}

/**
 * @brief Initialize the HTTP connection pool using CWIST_C1M_MODE.
 * @return 0 on success, -1 on failure.
 */
int cwist_http_pool_init(void) {
    const char *c1m = getenv("CWIST_C1M_MODE");
    bool use_c1m = true;
    if (c1m) {
        if (c1m[0] == '0' || strcmp(c1m, "false") == 0) {
            use_c1m = false;
        }
    }
    return cwist_http_pool_init_mode(use_c1m);
}

/**
 * @brief Initialize the HTTP connection pool in the requested mode.
 * @param use_c1m true for the event-driven reactor (C1M) pool, false for
 *        the classic dynamic thread pool.
 * @return 0 on success, -1 on failure (partially created state is cleaned up
 *         by cwist_http_pool_destroy()).
 */
int cwist_http_pool_init_mode(bool use_c1m) {
    atomic_store(&g_http_pool_stopping, false);
    g_http_thread_count = get_optimal_thread_count();

    if (use_c1m) {
        /* Pre-allocate reactor workers for async path (CWIST_C1M_MODE=1) */
        g_workers = cwist_alloc(g_http_thread_count * sizeof(http_thread_worker_t));
        if (!g_workers) return -1;
        g_worker_loads = cwist_alloc(g_http_thread_count * sizeof(_Atomic uint32_t));
        if (!g_worker_loads) {
            cwist_free(g_workers);
            g_workers = NULL;
            return -1;
        }
        for (int i = 0; i < g_http_thread_count; i++) atomic_init(&g_worker_loads[i], 0);
        g_rr_index = 0;
        memset(g_workers, 0, g_http_thread_count * sizeof(http_thread_worker_t));

        for (int i = 0; i < g_http_thread_count; i++) {
            g_workers[i].reactor = cwist_reactor_create();
            if (!g_workers[i].reactor) return -1;
            /* RX-uring receive path: route tagged RECV completions into the
             * HTTP connection state machine (no-op where unsupported). */
            cwist_reactor_set_rx_cb(g_workers[i].reactor, http_rx_recv_cb);
            g_workers[i].worker_id = (uint32_t)i;
            if (pthread_create(&g_workers[i].thread, NULL, http_pool_worker, &g_workers[i]) != 0) {
                return -1;
            }
        }
    } else {
        /* Initialize dynamic worker pool for classic path (CWIST_C1M_MODE=0) */
        g_dyn_pool.head = NULL;
        g_dyn_pool.tail = NULL;
        atomic_init(&g_dyn_pool.pending_tasks, 0);
        atomic_init(&g_dyn_pool.active_workers, 0);
        atomic_init(&g_dyn_pool.idle_workers, 0);
        atomic_init(&g_dyn_pool.max_workers, 65536);
        atomic_init(&g_dyn_pool.running, true);

        pthread_mutex_init(&g_dyn_pool.lock, NULL);
        pthread_cond_init(&g_dyn_pool.cond, NULL);

        /* Pre-warm worker threads. CWIST_POOL_PREWARM extends the spawn count
         * beyond the base pool when the expected concurrency is known, so the
         * acceptor does not serialize pthread_create + stack mmap during a
         * connection ramp. */
        long prewarm = g_http_thread_count;
        const char *pw = getenv("CWIST_POOL_PREWARM");
        if (pw && *pw) {
            char *end = NULL;
            long parsed = strtol(pw, &end, 10);
            if (end == pw || *end != '\0') parsed = 0;
            if (parsed > prewarm) prewarm = parsed;
        }
        long max_w = atomic_load_explicit(&g_dyn_pool.max_workers, memory_order_relaxed);
        if (prewarm > max_w) prewarm = max_w;
        for (long i = 0; i < prewarm; i++) {
            if (!http_spawn_worker()) {
                return -1;
            }
        }
    }
    return 0;
}

/**
 * @brief Queue a connection for handling on the classic dynamic pool.
 *
 * Enforces the inflight limit (over-limit connections get a bare 503 and are
 * closed) and spawns an extra worker when queued demand exceeds idle capacity.
 */
void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx) {
    long limit = cwist_http_inflight_limit();
    long inflight = atomic_fetch_add_explicit(&g_http_inflight, 1, memory_order_acq_rel) + 1;
    if (inflight > limit) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
#ifdef MSG_NOSIGNAL
        send(client_fd, CWIST_HTTP_503, sizeof(CWIST_HTTP_503) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
        close(client_fd);
        return;
    }

    http_pool_task_t *node = cwist_alloc(sizeof(*node));
    if (!node) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        close(client_fd);
        return;
    }
    node->client_fd = client_fd;
    node->handler_func = handler;
    node->ctx = ctx;
    node->next = NULL;

    pthread_mutex_lock(&g_dyn_pool.lock);
    if (!g_dyn_pool.tail) {
        g_dyn_pool.head = node;
        g_dyn_pool.tail = node;
    } else {
        g_dyn_pool.tail->next = node;
        g_dyn_pool.tail = node;
    }
    long pending =
        atomic_fetch_add_explicit(&g_dyn_pool.pending_tasks, 1, memory_order_release) + 1;

    /* Signalled sleepers remain counted idle until they reacquire this lock.
     * Compare queued demand with that capacity, not merely idle == 0: a
     * burst can otherwise strand connections behind busy keep-alive handlers.
     * Keep the cap check and the spawn reservation in this critical section. */
    long current = atomic_load_explicit(&g_dyn_pool.active_workers, memory_order_relaxed);
    long idle = atomic_load_explicit(&g_dyn_pool.idle_workers, memory_order_relaxed);
    long max_w = atomic_load_explicit(&g_dyn_pool.max_workers, memory_order_relaxed);
    if (pending > idle && current < max_w) {
        http_spawn_worker();
    }
    pthread_cond_signal(&g_dyn_pool.cond);
    pthread_mutex_unlock(&g_dyn_pool.lock);
}

/**
 * @brief Requeue the current connection's next handler turn on the pool.
 * @return false for invalid arguments (fd < 0 or NULL handler).
 */
bool cwist_http_pool_rearm_current(int client_fd, void (*handler)(int, void *), void *ctx) {
    if (client_fd < 0 || !handler) return false;
    cwist_http_pool_submit(client_fd, handler, ctx);
    return true;
}

/**
 * @brief Tear down both pool variants.
 *
 * Stops new work, drains queued tasks, retires the reactor generation and
 * waits for in-flight posters, then stops, joins, and frees the C1M reactors.
 */
void cwist_http_pool_destroy(void) {
    /* Queued continuations must release, not repost into a dying reactor. */
    atomic_store(&g_http_pool_stopping, true);
    atomic_store_explicit(&g_dyn_pool.running, false, memory_order_release);
    pthread_mutex_lock(&g_dyn_pool.lock);
    pthread_cond_broadcast(&g_dyn_pool.cond);
    pthread_mutex_unlock(&g_dyn_pool.lock);

    /* Drain tasks */
    pthread_mutex_lock(&g_dyn_pool.lock);
    http_pool_task_t *node = g_dyn_pool.head;
    g_dyn_pool.head = NULL;
    g_dyn_pool.tail = NULL;
    while (node) {
        http_pool_task_t *next = node->next;
        if (node->client_fd >= 0) close(node->client_fd);
        cwist_free(node);
        node = next;
    }
    pthread_mutex_unlock(&g_dyn_pool.lock);

    pthread_cond_destroy(&g_dyn_pool.cond);
    pthread_mutex_destroy(&g_dyn_pool.lock);

    /* Retire this generation of reactors before they go; see g_reactor_gen. */
    atomic_fetch_add(&g_reactor_gen, 1);
    while (atomic_load(&g_reactor_posters) > 0) sched_yield();

    if (g_workers) {
        for (int i = 0; i < g_http_thread_count; i++) {
            if (g_workers[i].reactor) {
                cwist_reactor_stop(g_workers[i].reactor);
            }
        }
        for (int i = 0; i < g_http_thread_count; i++) {
            pthread_join(g_workers[i].thread, NULL);
            if (g_workers[i].reactor) {
                cwist_reactor_destroy(g_workers[i].reactor);
                g_workers[i].reactor = NULL;
            }
        }
        cwist_free(g_workers);
        g_workers = NULL;
        if (g_worker_loads) {
            cwist_free(g_worker_loads);
            g_worker_loads = NULL;
        }
    }
}
/* --- End Thread Pool --- */

/* --- Async (one-shot, event-driven) connection path ------------------------
 * See include/cwist/net/http/http.h for the model overview.  The connection
 * shell lives across events; the recv stash is allocated lazily and released
 * whenever it drains to empty, so an idle keep-alive connection costs only
 * the shell plus its reactor slot. */

typedef struct {
    int client_fd;
    cwist_async_handler_t handler;
    void *ctx;
    cwist_reactor_t *reactor;
    cwist_http_async_conn_t *conn;
} http_async_ctx_t;

/* Defined below; referenced earlier by the RX-uring helpers. */
static void http_async_event_cb(int fd, void *ctx);
static bool http_async_stash_grow(cwist_http_async_conn_t *conn, size_t need);

/**
 * @brief Keep-alive idle timeout in seconds.
 * @return The validated CWIST_HTTP_KEEP_ALIVE_TIMEOUT env override or the
 *         CWIST_HTTP_KEEP_ALIVE_TIMEOUT_SEC default; cached after first use.
 */
static uint32_t cwist_http_keep_alive_timeout_sec(void) {
    static int cached_timeout = -1;
    if (cached_timeout < 0) {
        const char *env = getenv("CWIST_HTTP_KEEP_ALIVE_TIMEOUT");
        int val = 0;
        if (env && *env) {
            char *end = NULL;
            long v = strtol(env, &end, 10);
            if (end != env && *end == '\0' && v > 0 && v <= INT_MAX) val = (int)v;
        }
        cached_timeout = (val > 0) ? val : CWIST_HTTP_KEEP_ALIVE_TIMEOUT_SEC;
    }
    return (uint32_t)cached_timeout;
}

/**
 * @brief Release an async connection shell and its stashes.
 *
 * Decrements the worker load slot and the global inflight count, and frees
 * rbuf, obuf, and the shell itself. The fd is NOT closed here.
 */
static void http_async_conn_release(cwist_http_async_conn_t *conn) {
    if (!conn) return;
    if (g_worker_loads && conn->worker_id < (uint32_t)g_http_thread_count) {
        atomic_fetch_sub_explicit(&g_worker_loads[conn->worker_id], 1, memory_order_relaxed);
    }
    cwist_free(conn->rbuf);
    cwist_free(conn->obuf);
    cwist_free(conn);
    atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
}

/* Minimum free stash bytes required before arming a RECV SQE.  Below this
 * floor the legacy POLL + recv-drain path runs the stash growth instead. */
#define CWIST_RX_REARM_MIN_AVAIL 1024

/* Idle-keep-alive shrink floor for the rbuf/obuf stashes at rearm: an idle
 * connection holds ~KB, not 32 KB, while the grow-on-demand paths restore
 * capacity on the next busy turn. Must satisfy CWIST_RX_REARM_MIN_AVAIL so
 * the RECV-first arm stays available. */
#define CWIST_ASYNC_STASH_IDLE 4096

/** Arm the connection's next receive wait.  RX-uring first when the reactor
 * has a real io_uring ring: one RECV SQE into rbuf + len whose completion
 * drives the state machine, replacing the one-shot POLL + recv() pair.
 * Falls back to the legacy one-shot POLL on unsupported reactors, low stash
 * headroom, or submission failure (byte-identical legacy behavior).
 *
 * Wait-state invariant (owner thread only): at most one RECV SQE per
 * connection at a time, and a POLL is armed only when no RECV is in flight,
 * so the two can never double-dispatch.  The stash buffer never moves or is
 * consumed while a RECV referencing it is in flight; growth and compaction
 * happen only after the completion lands (rx_recv_inflight == false). */
static bool http_async_arm_wait(int fd, cwist_http_async_conn_t *conn,
                                cwist_async_handler_t handler, void *ctx,
                                cwist_reactor_t *reactor) {
    http_async_ctx_t next = {
        .client_fd = fd,
        .handler = handler,
        .ctx = ctx,
        .reactor = reactor,
        .conn = conn,
    };
    if (cwist_reactor_rx_supported(reactor) && !conn->peer_eof && !conn->rx_recv_inflight &&
        !conn->rx_prefers_poll) {
        if (conn->cap == 0 && !http_async_stash_grow(conn, CWIST_HTTP_READ_BUFFER_SIZE)) {
            /* Stash allocation failed: the legacy fill path retries growth
             * when POLL fires and closes on failure, matching today. */
        } else if (conn->cap > 0) {
            size_t avail = conn->cap - 1 - conn->len;
            if (avail >= CWIST_RX_REARM_MIN_AVAIL) {
                /* Mark the RECV in flight BEFORE submitting the SQE. The
                 * acceptor thread is not this reactor's owner, so the kernel
                 * can complete the SQE and the owner thread can dispatch the
                 * connection to close (and free it) while this function is
                 * still running; any conn write after submission is a
                 * use-after-free. Roll back on submission failure so the
                 * POLL fallback below sees the un-armed state. */
                conn->rx_recv_inflight = true;
                if (cwist_reactor_recv_arm(reactor, fd, conn->rbuf + conn->len, (unsigned)avail,
                                           conn, &conn->rx_armed_ns)) {
                    return true;
                }
                conn->rx_recv_inflight = false;
            }
        }
        /* Low headroom or arm failure: fall through to the POLL wait. */
    }
    return cwist_reactor_add(reactor, fd, http_async_event_cb, &next, sizeof(next));
}

/** Run the connection handler and act on its verdict.  Shared by the POLL
 * entry (http_async_event_cb) and the RX-uring RECV completion entry
 * (http_rx_recv_cb).  Takes over fd/conn ownership in every outcome. */
static void http_async_dispatch(int fd, cwist_http_async_conn_t *conn,
                                cwist_async_handler_t handler, void *ctx,
                                cwist_reactor_t *reactor) {
    cwist_async_action_t action = handler(fd, conn);

    if (action == CWIST_ASYNC_DEFER) {
        /* The handler parked the request on a cwist_async; the completion
         * path owns fd and conn now and re-arms or closes when done. */
        return;
    }
    if (action == CWIST_ASYNC_DETACH) {
        /* The handler owns fd now (h2c preface, protocol upgrade).  Free the
         * shell but never touch the fd. */
        if (g_worker_loads && conn->worker_id < (uint32_t)g_http_thread_count) {
            atomic_fetch_sub_explicit(&g_worker_loads[conn->worker_id], 1, memory_order_relaxed);
        }
        cwist_free(conn->rbuf);
        cwist_free(conn->obuf);
        cwist_free(conn);
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        return;
    }
    if (action == CWIST_ASYNC_CLOSE) {
        close(fd);
        http_async_conn_release(conn);
        return;
    }

    /* CWIST_ASYNC_REARM: keep the stashes allocated across keep-alive
     * requests to avoid per-request heap churn, but shrink an idle
     * connection's buffers toward a small floor. A served keep-alive
     * connection otherwise holds 16 KiB rbuf + 16 KiB obuf for its whole
     * idle lifetime (~24 KB RSS per connection at C1M scale); the
     * grow-on-demand paths (http_async_stash_grow, coalesce serialize)
     * restore capacity on the next busy turn. */
    if (conn->len == 0 && conn->cap > CWIST_ASYNC_STASH_IDLE) {
        char *nb = cwist_realloc(conn->rbuf, CWIST_ASYNC_STASH_IDLE);
        if (nb) {
            conn->rbuf = nb;
            conn->cap = CWIST_ASYNC_STASH_IDLE;
        }
    }
    if (conn->olen == 0 && conn->ocap > CWIST_ASYNC_STASH_IDLE) {
        char *nb = cwist_realloc(conn->obuf, CWIST_ASYNC_STASH_IDLE);
        if (nb) {
            conn->obuf = nb;
            conn->ocap = CWIST_ASYNC_STASH_IDLE;
        }
    }

    if (!http_async_arm_wait(fd, conn, handler, ctx, reactor)) {
        if (getenv("CWIST_ASYNC_DEBUG")) {
            static _Atomic long dbg_rearm_fail;
            long n = atomic_fetch_add(&dbg_rearm_fail, 1) + 1;
            if (n <= 5 || n % 10000 == 0)
                fprintf(stderr, "[async] rearm failed fd=%d total=%ld\n", fd, n);
        }
        close(fd);
        http_async_conn_release(conn);
    }
}

/** RX-uring RECV completion entry (see reactor_rx.h for the SQE namespace).
 * The completion IS the readiness signal: positive res staged bytes into the
 * stash, -EAGAIN means the POLL fallback takes over the wait, 0 is EOF, and
 * any other negative res is a recv error.  Runs on the reactor owner thread
 * while dispatching, so re-arms made here join the deferred SQE batch. */
static void http_rx_recv_cb(void *conn_ptr, int res) {
    cwist_http_async_conn_t *conn = (cwist_http_async_conn_t *)conn_ptr;
    conn->rx_recv_inflight = false;

    if (res == -ECANCELED) {
        /* The deferred submission failed (SQ wedged) or the ring tore down;
         * legacy arm-failure semantics: close fd and release the shell. */
        close(conn->fd);
        http_async_conn_release(conn);
        return;
    }
    if (res == 0) {
        /* EOF: do not re-arm; the handler drains complete buffered requests
         * and then closes through the existing peer_eof path. */
        conn->peer_eof = true;
    } else if (res < 0 && res != -EAGAIN) {
        close(conn->fd);
        http_async_conn_release(conn);
        return;
    } else if (res > 0) {
        conn->len += (size_t)res;
        conn->rbuf[conn->len] = '\0';
        conn->virgin = false;
    }
    /* res == -EAGAIN: nothing staged; the POLL fallback below resumes the
     * wait without running the handler on an empty stash. */

    /* Latency probe: rx_armed_ns is only written while the probe is enabled,
     * so a zero value means the probe is off and both clock reads below are
     * skipped.  Snapshot it now: the serve path re-arms the next RECV and
     * would overwrite conn->rx_armed_ns before the queue delay is computed. */
    uint64_t t0 = 0;
    uint64_t armed_ns = conn->rx_armed_ns;
    if (armed_ns) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    conn->last_active_sec = cwist_fast_monotonic_sec();

    if (res == -EAGAIN) {
        /* Data not ready: hand the wait to the legacy one-shot POLL.  Arm
         * POLL directly (NOT http_async_arm_wait, which is RECV-first and
         * would immediately re-arm a RECV, livelocking on -EAGAIN).  When
         * POLL fires, http_async_event_cb re-arms a RECV and the completion
         * drives from there.
         *
         * Learn: this client sends one request per idle period (wrk-style
         * keepalive), so an armed RECV just burns an SQE before the POLL
         * fallback every round.  Prefer POLL until a RECV actually stages
         * bytes again (pipelining detected), keeping the steady-state op
         * count identical to the legacy path. */
        conn->rx_prefers_poll = true;
        http_async_ctx_t next = {
            .client_fd = conn->fd,
            .handler = conn->handler,
            .ctx = conn->user_ctx,
            .reactor = conn->reactor,
            .conn = conn,
        };
        if (!cwist_reactor_add(conn->reactor, conn->fd, http_async_event_cb, &next, sizeof(next))) {
            close(conn->fd);
            http_async_conn_release(conn);
        }
        return;
    }

    /* rx_data_ready tells cwist_http_async_conn_fill the bytes are already
     * in the stash (or EOF is already recorded); never recv() on top of a
     * RECV completion.  Bytes flowed: a pipelining client gets the SQE
     * path again. */
    conn->rx_data_ready = true;
    conn->rx_prefers_poll = false;
    http_async_dispatch(conn->fd, conn, conn->handler, conn->user_ctx, conn->reactor);

    if (armed_ns) {
        uint64_t queue_us = (t0 - armed_ns) / 1000;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t t1 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        cwist_reactor_probe_record(conn->reactor, true, queue_us);
        cwist_reactor_probe_record(conn->reactor, false, (t1 - t0) / 1000);
    }
}

/* Engine-internal teardown for the async pool: drain pending receive-queue
 * bytes so the kernel answers with FIN instead of RST, then close and release.
 * Must run on the connection's reactor owner thread (the reactor may still
 * have the fd armed); not part of the public API. Defined below. */
static void http_async_close(int client_fd, cwist_http_async_conn_t *conn);

/** Reactor one-shot callback for an async connection.
 *
 * Reaps keep-alive connections past the idle timeout (draining first so the
 * teardown is a graceful FIN, not an RST), prefers an RX-uring RECV SQE over
 * POLL when the ring is free, and otherwise dispatches the connection handler
 * via http_async_dispatch().
 */
static void http_async_event_cb(int fd, void *ctx) {
    http_async_ctx_t *c = (http_async_ctx_t *)ctx;
    cwist_http_async_conn_t *conn = c->conn;
    cwist_async_handler_t handler = c->handler;

    uint32_t now = cwist_fast_monotonic_sec();
    uint32_t timeout_sec = cwist_http_keep_alive_timeout_sec();

    /* Idle connection reaper: close keep-alive sockets that exceeded timeout.
     * A client may have pipelined a request into the post-timeout,
     * pre-dispatch window; drain before closing so the teardown is a
     * graceful FIN, not an RST that flushes the peer's queued bytes. The
     * triggering one-shot slot is consumed, so closing here is safe. */
    if (conn->last_active_sec > 0 && (now - conn->last_active_sec) > timeout_sec) {
        http_async_close(fd, conn);
        return;
    }
    conn->last_active_sec = now;

    if (cwist_reactor_rx_supported(c->reactor) && !conn->rx_recv_inflight && !conn->peer_eof &&
        conn->len == 0 && !conn->rx_prefers_poll) {
        /* POLL is only a readiness trigger on the RX path: with an empty
         * stash the wait moves to a RECV SQE whose completion drives the
         * state machine.  (POLL gets armed only by the -EAGAIN and
         * low-headroom fallbacks.)  Buffered bytes (continuation posts,
         * parked-writer resumes) must be served, not waited on, so they
         * fall through to the legacy dispatch below.  If the arm fails,
         * fall through to the legacy inline drain. */
        if (http_async_arm_wait(fd, conn, handler, c->ctx, c->reactor)) return;
    }

    http_async_dispatch(fd, conn, handler, c->ctx, c->reactor);
}

typedef struct {
    cwist_reactor_post_t post;
    http_async_ctx_t next;
} http_async_continuation_t;

/**
 * @brief Reactor-posted continuation: resume a pipelined async connection.
 *
 * Runs on the target reactor thread; closes instead of serving once the app
 * or the pool is stopping.
 */
static void http_async_continue(void *ctx) {
    http_async_continuation_t *continuation = ctx;
    http_async_ctx_t next = continuation->next;
    cwist_free(continuation);
    if (!atomic_load(&g_cwist_running) || atomic_load(&g_http_pool_stopping)) {
        http_async_close(next.client_fd, next.conn);
        return;
    }
    http_async_event_cb(next.client_fd, &next);
}

/**
 * @brief Re-arm an async connection for its next event.
 *
 * Handles deferred-completion shutdown races, peer EOF, pipelined bytes
 * (posted as a continuation so other connections run between batches), and
 * RECV-first re-arm with a POLL fallback.
 * @return false when the connection was closed or the re-arm failed.
 */
bool cwist_http_async_rearm(int client_fd, cwist_reactor_t *reactor,
                            cwist_http_async_conn_t *conn) {
    if (client_fd < 0 || !reactor || !conn) return false;
    /* A deferred response may finish in reactor_destroy's final drain. Never
     * enqueue new work into that final snapshot or the connection is orphaned. */
    if (atomic_load(&g_http_pool_stopping)) {
        http_async_close(client_fd, conn);
        return false;
    }
    if (conn->peer_eof && conn->len == 0) {
        http_async_close(client_fd, conn);
        return false;
    }
    conn->last_active_sec = cwist_fast_monotonic_sec();
    http_async_ctx_t next = {
        .client_fd = client_fd,
        .handler = conn->handler,
        .ctx = conn->user_ctx,
        .reactor = reactor,
        .conn = conn,
    };
    if (conn->len > 0) {
        /* Pipelined bytes already sit in the stash: waiting for POLLIN would
         * hang. Post instead of recursing inline, so ready connections can
         * run between bounded HTTP request batches. */
        http_async_continuation_t *continuation = cwist_alloc(sizeof(*continuation));
        if (!continuation) {
            http_async_close(client_fd, conn);
            return false;
        }
        continuation->next = next;
        continuation->post = (cwist_reactor_post_t){
            .cb = http_async_continue,
            .ctx = continuation,
        };
        if (!cwist_reactor_post(reactor, &continuation->post)) {
            long n =
                atomic_fetch_add_explicit(&g_http_continuation_shed, 1, memory_order_relaxed) + 1;
            cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_HTTP_CONTINUATION_SHED);
            if (getenv("CWIST_ASYNC_DEBUG") && (n <= 5 || n % 10000 == 0))
                fprintf(stderr, "[async] continuation shed fd=%d total=%ld\n", client_fd, n);
            cwist_free(continuation);
            http_async_close(client_fd, conn);
            return false;
        }
        return true;
    }
    /* Same stash shrink as the REARM path in http_async_event_cb. */
    if (conn->len == 0 && conn->cap > 65536) {
        cwist_free(conn->rbuf);
        conn->rbuf = NULL;
        conn->cap = 0;
    }
    /* RX-uring: RECV-first re-arm when the ring supports it (peer_eof was
     * handled above, so the wait is a plain receive wait).  arm_wait falls
     * back to the legacy one-shot POLL on unsupported reactors or arm
     * failure. */
    if (!http_async_arm_wait(client_fd, conn, conn->handler, conn->user_ctx, reactor)) {
        close(client_fd);
        http_async_conn_release(conn);
        return false;
    }
    return true;
}

/** Close an async connection: drain pending receive-queue bytes (so the
 * kernel answers with FIN instead of RST, mirroring
 * https_connection_teardown()), close the fd, and release the connection
 * shell.  Must run on the connection's reactor owner thread.
 */
static void http_async_close(int client_fd, cwist_http_async_conn_t *conn) {
    if (client_fd >= 0) {
        /* close() on a socket with unread receive-queue data makes the
         * kernel answer with RST instead of a graceful FIN. A client can
         * pipeline its next request onto a connection the server is
         * closing, leaving those bytes unconsumed here; under connection
         * churn that turned most teardowns into abortive closes (the RST
         * also flushes response bytes the peer had not read yet). Drain
         * whatever is pending before closing; the socket is about to be
         * destroyed anyway. Mirrors https_connection_teardown(). */
        char drain[2048];
        ssize_t n;
        int guard = 16;
        while (guard-- > 0 && (n = recv(client_fd, drain, sizeof(drain), MSG_DONTWAIT)) > 0) {
            /* discard */
        }
        (void)n;
        close(client_fd);
    }
    http_async_conn_release(conn);
}

/**
 * @brief Close an async connection whose owning worker pool is gone.
 *
 * Marks the load slot index invalid so the teardown does not touch load
 * counters that may belong to a later pool generation.
 */
void cwist_http_async_close_orphan(int client_fd, cwist_http_async_conn_t *conn) {
    /* The worker that owned conn is gone, and its load slot index may belong
     * to a later pool by now: leave the load counters alone. */
    if (conn) conn->worker_id = UINT32_MAX;
    http_async_close(client_fd, conn);
}

/**
 * @brief Submit a connection to the C1M async reactor pool.
 *
 * Enforces the inflight limit (503 + close on overflow), sets the socket
 * non-blocking, allocates the connection shell, and load-balances with
 * power-of-two-choices over the per-worker load counters.
 * @return false when the connection was rejected or the initial arm failed.
 */
bool cwist_http_pool_submit_async(int client_fd, cwist_async_handler_t handler, void *ctx) {
    long limit = cwist_http_inflight_limit();
    long inflight = atomic_fetch_add_explicit(&g_http_inflight, 1, memory_order_acq_rel) + 1;
    if (inflight > limit) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
#ifdef MSG_NOSIGNAL
        send(client_fd, CWIST_HTTP_503, sizeof(CWIST_HTTP_503) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
        close(client_fd);
        return false;
    }

    /* The one-shot path must never park the reactor on a blocking recv. */
    int fl = fcntl(client_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);

    cwist_http_async_conn_t *conn = cwist_alloc(sizeof(*conn));
    if (!conn) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        close(client_fd);
        return false;
    }
    memset(conn, 0, sizeof(*conn));
    conn->fd = client_fd;
    conn->user_ctx = ctx;
    conn->virgin = true;
    conn->last_active_sec = cwist_fast_monotonic_sec();

    /* Worker selection: Power of Two Random Choices (P2C) load balancing
     * to eliminate queue skew without cache-bouncing work stealing. */
    size_t worker_idx;
    if (g_worker_loads && g_http_thread_count > 1) {
        worker_idx = cwist_sched_p2c_select_worker(g_worker_loads, (uint32_t)g_http_thread_count);
    } else {
        worker_idx = g_rr_index;
        g_rr_index = (g_rr_index + 1) % (size_t)g_http_thread_count;
    }
    http_thread_worker_t *w = &g_workers[worker_idx];

    conn->worker_id = (uint32_t)worker_idx;
    if (g_worker_loads) {
        atomic_fetch_add_explicit(&g_worker_loads[worker_idx], 1, memory_order_relaxed);
    }
    conn->reactor = w->reactor;
    conn->handler = handler;

    /* RX-uring: when the worker reactor has a real io_uring ring, the first
     * receive wait is a RECV SQE into the (freshly grown) stash whose
     * completion drives the state machine; on any arm failure the legacy
     * one-shot POLL is armed instead. */
    if (http_async_arm_wait(client_fd, conn, handler, ctx, w->reactor)) return true;

    if (getenv("CWIST_ASYNC_DEBUG")) {
        static _Atomic long dbg_submit_fail;
        long n = atomic_fetch_add(&dbg_submit_fail, 1) + 1;
        if (n <= 5 || n % 10000 == 0)
            fprintf(stderr, "[async] submit-add failed fd=%d total=%ld worker=%zu\n", client_fd, n,
                    worker_idx);
    }
    http_async_conn_release(conn);
    close(client_fd);
    return false;
}

/* --- End Async Connection Path --- */

/**
 * @file http.c
 * @brief Core HTTP request/response allocation, serialization, socket, and server-loop helpers.
 */

const int CWIST_CREATE_SOCKET_FAILED = -1;
const int CWIST_HTTP_UNAVAILABLE_ADDRESS = -2;
const int CWIST_HTTP_BIND_FAILED = -3;
const int CWIST_HTTP_SETSOCKOPT_FAILED = -4;
const int CWIST_HTTP_LISTEN_FAILED = -5;

/* --- Helpers --- */

/**
 * @brief Convert a HTTP method enum into its wire-format token.
 * @param method HTTP method enum value.
 * @return Static string name for the method.
 */
const char *cwist_http_method_to_string(cwist_http_method_t method) {
    switch (method) {
        case CWIST_HTTP_GET: return "GET";
        case CWIST_HTTP_POST: return "POST";
        case CWIST_HTTP_PUT: return "PUT";
        case CWIST_HTTP_DELETE: return "DELETE";
        case CWIST_HTTP_PATCH: return "PATCH";
        case CWIST_HTTP_HEAD: return "HEAD";
        case CWIST_HTTP_OPTIONS: return "OPTIONS";
        case CWIST_HTTP_CONNECT: return "CONNECT";
        default: return "UNKNOWN";
    }
}

#define MAKE_MAGIC4(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define MAGIC_GET MAKE_MAGIC4('G', 'E', 'T', ' ')
#define MAGIC_POST MAKE_MAGIC4('P', 'O', 'S', 'T')
#define MAGIC_PUT MAKE_MAGIC4('P', 'U', 'T', ' ')
#define MAGIC_DELE MAKE_MAGIC4('D', 'E', 'L', 'E')
#define MAGIC_HEAD MAKE_MAGIC4('H', 'E', 'A', 'D')

/**
 * @brief Parse a method token (with explicit length) into a method enum.
 *
 * Uses a SWAR 4-byte magic fast path for GET/POST/PUT/DELETE/HEAD, then
 * exact-length comparisons.
 * @return Matching method, or CWIST_HTTP_UNKNOWN.
 */
cwist_http_method_t cwist_http_string_to_method_len(const char *str, size_t len) {
    if (!str || len == 0) return CWIST_HTTP_UNKNOWN;

    /* Ultra-fast path: SWAR 4-byte magic lookup for GET, POST, PUT, DELETE, HEAD */
    if (len >= 3) {
        uint32_t m = 0;
        memcpy(&m, str, sizeof(uint32_t));
        switch (m) {
            case MAGIC_GET: return CWIST_HTTP_GET;
            case MAGIC_POST: return CWIST_HTTP_POST;
            case MAGIC_PUT: return CWIST_HTTP_PUT;
            case MAGIC_DELE:
                if (len >= 6 && memcmp(str, "DELETE", 6) == 0) return CWIST_HTTP_DELETE;
                break;
            case MAGIC_HEAD:
                if (len == 4) return CWIST_HTTP_HEAD;
                break;
            default: break;
        }
    }

    if (len == 3) {
        uint32_t v = 0;
        memcpy(&v, str, 3);
        if ((v & 0x00FFFFFF) == 0x00544547) return CWIST_HTTP_GET;
        if ((v & 0x00FFFFFF) == 0x00545550) return CWIST_HTTP_PUT;
    } else if (len == 4) {
        uint32_t v = 0;
        memcpy(&v, str, 4);
        if (v == 0x54534F50) return CWIST_HTTP_POST;
        if (v == 0x44414548) return CWIST_HTTP_HEAD;
    } else if (len == 5) {
        if (memcmp(str, "PATCH", 5) == 0) return CWIST_HTTP_PATCH;
    } else if (len == 6) {
        if (memcmp(str, "DELETE", 6) == 0) return CWIST_HTTP_DELETE;
    } else if (len == 7) {
        if (memcmp(str, "OPTIONS", 7) == 0) return CWIST_HTTP_OPTIONS;
    }
    return CWIST_HTTP_UNKNOWN;
}

/**
 * @brief Parse a NUL-terminated method string into a method enum.
 * @return Matching method, or CWIST_HTTP_UNKNOWN for NULL/unrecognized input.
 */
cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (!method_str) return CWIST_HTTP_UNKNOWN;
    return cwist_http_string_to_method_len(method_str, strlen(method_str));
}

/* --- Header Manipulation --- */

/**
 * @brief Allocate a fixed-size struct from an arena, falling back to the heap.
 * @param arena Request/response arena, or NULL for plain heap allocation.
 * @param size Struct size in bytes.
 * @return Zeroed memory block, or NULL when both paths fail.
 */
static void *cwist_http_struct_alloc(cwist_arena_t *arena, size_t size) {
    void *p = arena ? cwist_arena_alloc(arena, size) : NULL;
    if (p) {
        memset(p, 0, size);
        return p;
    }
    return cwist_alloc(size);
}

/**
 * @brief Create an sstring whose struct lives in an arena when possible.
 *
 * The character buffer still comes from the heap (cwist_realloc); only the
 * fixed-size cwist_sstring struct is bump-allocated. Arena-owned structs get
 * owns_storage = false so cwist_sstring_destroy releases the buffer but
 * leaves the struct to the arena.
 */
static cwist_sstring *cwist_http_sstring_create(cwist_arena_t *arena) {
    bool from_arena = false;
    cwist_sstring *str = NULL;
    if (arena) {
        str = (cwist_sstring *)cwist_arena_alloc(arena, sizeof(cwist_sstring));
        if (str) from_arena = true;
    }
    if (!str) {
        str = (cwist_sstring *)cwist_alloc(sizeof(cwist_sstring));
    }
    if (!str) return NULL;

    memset(str, 0, sizeof(cwist_sstring));
    str->is_fixed = false;
    str->owns_storage = !from_arena;
    str->size = 0;
    str->data = NULL;
    str->get_size = cwist_sstring_get_size;
    str->compare = cwist_sstring_compare_sstring;
    str->copy = cwist_sstring_copy_sstring;
    str->append = cwist_sstring_append_sstring;

    return str;
}

/**
 * @brief Assign into an sstring, carving the character buffer from an arena.
 *
 * Arena-backed buffers are marked as borrowed so sstring teardown never
 * frees them individually (the arena releases everything in one shot) and a
 * later mutation transparently detaches to the heap. Falls back to a regular
 * heap assign when the arena is NULL or exhausted.
 */
static cwist_error_t cwist_http_sstring_assign_arena(cwist_sstring *str, cwist_arena_t *arena,
                                                     const char *data, size_t len) {
    if (arena) {
        char *buf = (char *)cwist_arena_alloc(arena, len + 1);
        if (buf) {
            if (str->data && !str->borrows_buffer) {
                cwist_free(str->base ? str->base : str->data);
            }
            if (data && len > 0) memcpy(buf, data, len);
            buf[len] = '\0';
            str->base = NULL;
            str->data = buf;
            str->size = len;
            str->capacity = 0; ///< arena capacity is unknown until detach
            str->borrows_buffer = true;
            cwist_error_t err = make_error(CWIST_ERR_INT8);
            err.error.err_i8 = ERR_SSTRING_OKAY;
            return err;
        }
    }
    return cwist_sstring_assign_len(str, data, len);
}

/**
 * @brief Prepend one header node with length, optionally bump-allocated from an arena.
 */
static cwist_error_t cwist_http_header_add_ex_len(cwist_http_header_node **head,
                                                  cwist_arena_t *arena, const char *key,
                                                  size_t key_len, const char *value,
                                                  size_t value_len) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    bool from_arena = false;
    cwist_http_header_node *node = NULL;
    if (arena) {
        node = (cwist_http_header_node *)cwist_arena_alloc(arena, sizeof(cwist_http_header_node));
        if (node) {
            memset(node, 0, sizeof(cwist_http_header_node));
            from_arena = true;
        }
    }
    if (!node) {
        node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    }
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }
    node->arena_owned = from_arena;

    node->key = cwist_http_sstring_create(arena);
    node->value = cwist_http_sstring_create(arena);
    node->next = NULL;

    bool ok = node->key && node->value;
    if (ok) {
        cwist_error_t kerr = cwist_http_sstring_assign_arena(node->key, arena, key, key_len);
        ok = cwist_error_is_ok(&kerr);
        cwist_error_dispose(&kerr);
    }
    if (ok) {
        cwist_error_t verr = cwist_http_sstring_assign_arena(node->value, arena, value, value_len);
        ok = cwist_error_is_ok(&verr);
        cwist_error_dispose(&verr);
    }
    if (!ok) {
        /* A half-built node would serialize as an empty or missing header;
         * release it and report the failure instead. */
        cwist_http_header_free_all(node);
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error",
                                "Failed to allocate header strings");
        return err;
    }

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend one header node, optionally bump-allocated from an arena.
 * @param head Header-list head pointer to update.
 * @param arena Arena to carve the node from, or NULL for heap allocation.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error describing success or allocation failure.
 */
static cwist_error_t cwist_http_header_add_ex(cwist_http_header_node **head, cwist_arena_t *arena,
                                              const char *key, const char *value) {
    return cwist_http_header_add_ex_len(head, arena, key, key ? strlen(key) : 0, value,
                                        value ? strlen(value) : 0);
}

/**
 * @brief Prepend a header whose key/value live in static storage (zero-copy).
 *
 * Used for compile-time constant headers such as the default security set:
 * the node and sstring structs come from the arena and the character bytes
 * are borrowed, so a default response pays no heap traffic for headers at
 * all. Key and value must outlive the header list.
 */
static cwist_error_t cwist_http_header_add_static(cwist_http_header_node **head,
                                                  cwist_arena_t *arena, const char *key,
                                                  const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    bool from_arena = false;
    cwist_http_header_node *node = NULL;
    if (arena) {
        node = (cwist_http_header_node *)cwist_arena_alloc(arena, sizeof(cwist_http_header_node));
        if (node) {
            memset(node, 0, sizeof(cwist_http_header_node));
            from_arena = true;
        }
    }
    if (!node) {
        node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    }
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }
    node->arena_owned = from_arena;

    node->key = cwist_http_sstring_create(arena);
    node->value = cwist_http_sstring_create(arena);
    node->next = NULL;

    if (!node->key || !node->value) {
        if (!from_arena) cwist_free(node);
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error",
                                "Failed to allocate header strings");
        return err;
    }

    cwist_sstring_borrow(node->key, key, strlen(key));
    cwist_sstring_borrow(node->value, value, strlen(value));

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/** @brief True when a header name or value contains no CR or LF. */
static bool cwist_http_header_text_is_safe(const char *text) {
    return !text || !strpbrk(text, "\r\n");
}

/**
 * @brief Prepend one header node to the linked-list header collection.
 * @param head Header-list head pointer to update.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error: INT16 0 on success, INT16 -1 when head is NULL or
 *         key/value contains CR or LF (nothing is added), or a JSON error on
 *         allocation failure.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                    const char *value) {
    /* The serializer writes key and value verbatim around ": " and CRLF, so a
     * CR or LF here would let caller-supplied data end the header early and
     * inject further headers or a body into the response. */
    if (!head || !cwist_http_header_text_is_safe(key) || !cwist_http_header_text_is_safe(value)) {
        cwist_error_t err = make_error(CWIST_ERR_INT16);
        err.error.err_i16 = -1;
        return err;
    }
    return cwist_http_header_add_ex(head, NULL, key, value);
}

/**
 * @brief Find a header value using case-insensitive header-name comparison.
 * @param head Head of the header linked list.
 * @param key Header name to search for.
 * @return Raw header value string, or NULL when absent.
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    if (!head || !key) return NULL;
    size_t klen = strlen(key);
    cwist_http_header_node *curr = head;
    while (curr) {
        if (curr->key && curr->key->data && curr->key->size == klen) {
            if (strcasecmp(curr->key->data, key) == 0) {
                return curr->value ? curr->value->data : NULL;
            }
        }
        curr = curr->next;
    }
    return NULL;
}

/**
 * @brief Unlink and release every header node matching a name.
 * @param head Pointer to the header-list head; updated as nodes are removed.
 * @param key Header name (case-insensitive) to remove; all duplicates go.
 * @return Number of nodes removed.
 *
 * Ownership is handled per node: heap-owned nodes are freed, arena-owned
 * nodes are left for the arena to reclaim, and borrowed key/value buffers
 * are never freed individually.  Applications should call this instead of
 * hand-rolling unlink loops, which have repeatedly gotten that bookkeeping
 * wrong.
 */
size_t cwist_http_header_remove(cwist_http_header_node **head, const char *key) {
    if (!head || !key) return 0;
    size_t removed = 0;
    while (*head) {
        cwist_http_header_node *node = *head;
        if (node->key && node->key->data && strcasecmp(node->key->data, key) == 0) {
            *head = node->next;
            cwist_sstring_destroy(node->key);
            cwist_sstring_destroy(node->value);
            if (!node->arena_owned) cwist_free(node);
            removed++;
        } else {
            head = &node->next;
        }
    }
    return removed;
}

/**
 * @brief Add default security headers to an HTTP response if not already present.
 *
 * Safe to call for both HTTP and HTTPS responses.  HSTS is intentionally
 * omitted here because RFC 6797 section 7.2 forbids sending it over plain HTTP;
 * browsers ignore it on non-TLS connections anyway.  Use
 * cwist_http_response_add_hsts() from your HTTPS handler to add it there.
 *
 * @param res Response object to populate.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res) {
    if (!res) return;
    cwist_arena_t *arena = (cwist_arena_t *)res->arena;

    /* All key/value pairs are compile-time constants: borrow them instead of
     * heap-copying, so a default response performs zero heap allocations for
     * its security headers (arena carve only). */
    if (!cwist_http_header_get(res->headers, "X-Frame-Options")) {
        cwist_http_header_add_static(&res->headers, arena, "X-Frame-Options", "DENY");
    }
    if (!cwist_http_header_get(res->headers, "X-Content-Type-Options")) {
        cwist_http_header_add_static(&res->headers, arena, "X-Content-Type-Options", "nosniff");
    }
    if (!cwist_http_header_get(res->headers, "Referrer-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Referrer-Policy",
                                     "strict-origin-when-cross-origin");
    }
    if (!cwist_http_header_get(res->headers, "Content-Security-Policy")) {
        cwist_http_header_add_static(
            &res->headers, arena, "Content-Security-Policy",
            "default-src 'self'; "
            "script-src 'self' https://cdnjs.cloudflare.com; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; "
            "font-src 'self' https://fonts.gstatic.com https://cdn.jsdelivr.net; "
            "img-src 'self'; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self'; "
            "object-src 'none';");
    }
    if (!cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Cross-Origin-Resource-Policy",
                                     "same-origin");
    }
    /* Permissions-Policy (W3C Permissions Policy Level 2) — deny access to
     * sensitive browser APIs that CWIST apps almost never need.  Callers that
     * require a specific feature can set the header before calling this
     * function; the existing-header check below will skip the default. */
    if (!cwist_http_header_get(res->headers, "Permissions-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Permissions-Policy",
                                     "camera=(), microphone=(), geolocation=(), payment=(), "
                                     "usb=(), interest-cohort=()");
    }
    if (!cwist_http_header_get(res->headers, "Cross-Origin-Opener-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Cross-Origin-Opener-Policy", "same-origin");
    }
}

/**
 * @brief Add Strict-Transport-Security to a TLS response (HTTPS only).
 *
 * RFC 6797 section 7.2 prohibits HSTS over plain HTTP.  Call this only from an
 * HTTPS handler, after cwist_http_response_add_security_headers().
 * No-op when the header is already present.
 *
 * @param res Response object to populate.
 */
void cwist_http_response_add_hsts(cwist_http_response *res) {
    if (!res) return;
    cwist_arena_t *arena = (cwist_arena_t *)res->arena;
    if (!cwist_http_header_get(res->headers, "Strict-Transport-Security")) {
        cwist_http_header_add_static(&res->headers, arena, "Strict-Transport-Security",
                                     "max-age=31536000; includeSubDomains");
    }
}

/**
 * @brief Destroy every node in a request or response header list.
 * @param head Head of the header linked list.
 */
void cwist_http_header_free_all(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        cwist_http_header_node *next = curr->next;
        cwist_sstring_destroy(curr->key);
        cwist_sstring_destroy(curr->value);
        if (!curr->arena_owned) {
            cwist_free(curr);
        }
        curr = next;
    }
}

/**
 * @brief Copy the cached RFC 7231 IMF-fixdate string for the current second.
 * @param out_buf Destination buffer; receives 29 date bytes plus NUL.
 */
static void cwist_get_cached_date_header(char out_buf[36]) {
    static _Atomic time_t g_last_sec = 0;
    static char g_date_str[36] = {0};
    static pthread_mutex_t g_date_lock = PTHREAD_MUTEX_INITIALIZER;

    time_t now = time(NULL);
    time_t last = atomic_load_explicit(&g_last_sec, memory_order_relaxed);
    if (now != last) {
        pthread_mutex_lock(&g_date_lock);
        if (now != atomic_load_explicit(&g_last_sec, memory_order_relaxed)) {
            struct tm gmt;
#if defined(_WIN32)
            gmtime_s(&gmt, &now);
#else
            gmtime_r(&now, &gmt);
#endif
            strftime(g_date_str, sizeof(g_date_str), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
            atomic_store_explicit(&g_last_sec, now, memory_order_release);
        }
        pthread_mutex_unlock(&g_date_lock);
    }
    memcpy(out_buf, g_date_str, 30);
    out_buf[29] = '\0';
}

/* --- Request Lifecycle --- */

/**
 * @brief Allocate and initialize a default HTTP request object.
 * @return Newly allocated request, or NULL on allocation failure.
 */
cwist_http_request *cwist_http_request_create(void) {
    cwist_arena_t *arena = cwist_arena_create(0);
    cwist_http_request *req =
        (cwist_http_request *)cwist_http_struct_alloc(arena, sizeof(cwist_http_request));
    if (!req) {
        cwist_arena_destroy(arena);
        return NULL;
    }

    req->method = CWIST_HTTP_GET; // Default
    req->path = cwist_http_sstring_create(arena);
    req->query = cwist_http_sstring_create(arena);
    req->query_params = NULL;
    req->path_params = NULL;
    req->version = cwist_http_sstring_create(arena);
    req->headers = NULL;
    req->body = cwist_http_sstring_create(arena);
    req->keep_alive = true;
    req->client_fd = -1;
    req->app = NULL;
    req->db = NULL;
    req->flash = NULL;
    req->upgraded = false;
    req->content_length = 0;
    req->private_data = NULL;
    req->endpoint_opts = CWIST_ENDPOINT_DEFAULT;

    // Defaults (borrowed statics; parsing overwrites them via arena/heap assign)
    cwist_sstring_borrow(req->version, "HTTP/1.1", 8);
    cwist_sstring_borrow(req->path, "/", 1);

    return req;
}

/* --- Request Data Processing */

/**
 * @brief Resolve the peer IP address for a connected client socket.
 * @param fd Connected client socket descriptor.
 * @return Heap-allocated string containing the textual IP address.
 */
/**
 * @brief Format a time_t as an HTTP-date (RFC 7231).
 * @param t Unix timestamp.
 * @param buf Output buffer.
 * @param len Buffer capacity.
 */
void cwist_http_format_date(time_t t, char *buf, size_t len) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/**
 * @brief Parse an HTTP-date string into a time_t.
 * @param str HTTP-date string.
 * @return Parsed timestamp, or (time_t)-1 on failure.
 */
time_t cwist_http_parse_date(const char *str) {
    if (!str) return (time_t)-1;
    struct tm tm = {0};
    const char *fmt = "%a, %d %b %Y %H:%M:%S %Z";
    if (strptime(str, fmt, &tm) == NULL) {
        // Try alternative formats
        fmt = "%a, %d-%b-%y %H:%M:%S %Z";
        if (strptime(str, fmt, &tm) == NULL) {
            fmt = "%a %b %d %H:%M:%S %Y";
            if (strptime(str, fmt, &tm) == NULL) {
                return (time_t)-1;
            }
        }
    }
    return timegm(&tm);
}

/**
 * @brief Resolve the peer IP address for a connected client socket.
 * @param fd Connected client socket descriptor.
 * @return New sstring with the textual IP; "127.0.0.1" when unavailable.
 */
cwist_sstring *cwist_get_client_ip_from_fd(int fd) {
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "127.0.0.1");
    // First, check if fd is available
    // If unavailable, return localhost
    if (fd <= 0) return s;

#ifndef CWIST_WASI_NO_SOCKETS
    /* WASI preview1 has no socket peer addresses; the caller only needs a
     * placeholder when running under a WASM host. */
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    // get client info
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == -1) {
        fprintf(stdout, "[ERROR] Failed to get client info from file descriptor");
        return s;
    }

    char ip[INET6_ADDRSTRLEN];

    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
    }

    // assign found ip as a value
    cwist_sstring_assign(s, ip);
#endif
    return s;
}

/**
 * @brief Destroy a parsed HTTP request and all nested allocations it owns.
 * @param req Request object to destroy.
 */
void cwist_http_request_destroy(cwist_http_request *req) {
    if (req) {
        cwist_arena_t *arena = (cwist_arena_t *)req->arena;
        cwist_sstring_destroy(req->path);
        cwist_sstring_destroy(req->query);
        cwist_query_map_destroy(req->query_params);
        cwist_query_map_destroy(req->path_params);
        cwist_sstring_destroy(req->version);
        cwist_sstring_destroy(req->body);
        cwist_query_map_destroy(req->flash);
        cwist_session_destroy(req->session);
        cwist_free(req->csrf_token);
        cwist_http_header_free_all(req->headers);
        if (!arena || !cwist_arena_owns(arena, req)) {
            cwist_free(req);
        }
        /* Releases every arena-carved struct (request, sstrings, header
         * nodes) in one shot; heap-owned buffers were freed above. */
        cwist_arena_destroy(arena);
    }
}

/* --- Response Lifecycle --- */

static void cwist_http_response_release_file_stream(cwist_http_response *res) {
    if (!res || !res->use_file_stream) return;
    if (res->file_stream_auto_close && res->file_stream_fd >= 0) {
        close(res->file_stream_fd);
    }
    res->use_file_stream = false;
    res->file_stream_fd = -1;
    res->file_stream_len = 0;
    res->file_stream_offset = 0;
    res->file_stream_auto_close = false;
}

static void cwist_http_response_release_ptr_body(cwist_http_response *res) {
    if (!res || !res->is_ptr_body) return;
    if (res->ptr_body_cleanup && res->ptr_body) {
        res->ptr_body_cleanup(res->ptr_body, res->ptr_body_len, res->ptr_body_cleanup_ctx);
    }
    res->is_ptr_body = false;
    res->ptr_body = NULL;
    res->ptr_body_len = 0;
    res->ptr_body_cleanup = NULL;
    res->ptr_body_cleanup_ctx = NULL;
}

cwist_http_response *cwist_http_response_create(void) {
    cwist_http_response *res = (cwist_http_response *)malloc(sizeof(cwist_http_response));
    if (!res) return NULL;

    res->version = cwist_http_sstring_create(arena);
    res->status_code = CWIST_HTTP_OK;
    res->status_text = cwist_http_sstring_create(arena);
    res->headers = NULL;
    res->body = cwist_sstring_create();
    res->endpoint_opts = CWIST_ENDPOINT_DEFAULT;
    res->keep_alive = true;
    res->is_ptr_body = false;
    res->ptr_body = NULL;
    res->ptr_body_len = 0;
    res->ptr_body_cleanup = NULL;
    res->ptr_body_cleanup_ctx = NULL;
    res->use_file_stream = false;
    res->file_stream_fd = -1;
    res->file_stream_len = 0;
    res->file_stream_offset = 0;
    res->file_stream_auto_close = false;

    // Defaults (borrowed statics; handlers may overwrite via regular assign)
    cwist_sstring_borrow(res->version, "HTTP/1.1", 8);
    cwist_sstring_borrow(res->status_text, "OK", 2);

    return res;
}

/**
 * @brief Allocate a response backed by its own arena.
 * @return Newly allocated response, or NULL on failure.
 */
cwist_http_response *cwist_http_response_create(void) {
    cwist_arena_t *arena = cwist_arena_create(0);
    cwist_http_response *res = cwist_http_response_create_impl(arena, false);
    if (!res) {
        cwist_arena_destroy(arena);
        return NULL;
    }
    return res;
}

/**
 * @brief Allocate a response carved from a caller-owned arena.
 * @param arena Arena to allocate from; NULL falls back to
 *        cwist_http_response_create().
 */
cwist_http_response *cwist_http_response_create_in_arena(void *arena) {
    if (!arena) return cwist_http_response_create();
    return cwist_http_response_create_impl((cwist_arena_t *)arena, true);
}

/**
 * @brief Destroy an HTTP response and release any attached body resources.
 * @param res Response object to destroy.
 */
void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        cwist_http_response_release_ptr_body(res);
        cwist_http_response_release_file_stream(res);
        cwist_sstring_destroy(res->version);
        cwist_sstring_destroy(res->status_text);
        cwist_sstring_destroy(res->body);
        cwist_sstring_destroy(res->stream_buf);
        cwist_http_header_free_all(res->headers);
        cwist_free(res->alt_svc);
        /* Snapshot before the possible free: when the arena was full the
         * struct fell back to the heap, so res may be freed below and any
         * later field read is a use-after-free. */
        bool borrowed = res->arena_borrowed;
        if (!arena || !cwist_arena_owns(arena, res)) {
            cwist_free(res);
        }
        if (!borrowed) {
            cwist_arena_destroy(arena);
        }
    }
}

void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len) {
    cwist_http_response_set_body_ptr_managed(res, ptr, len, NULL, NULL);
}

void cwist_http_response_set_body_ptr_managed(cwist_http_response *res, const void *ptr, size_t len, cwist_http_body_cleanup_fn cleanup, void *ctx) {
    if (!res) return;
    cwist_http_response_release_file_stream(res);
    cwist_http_response_release_ptr_body(res);
    res->is_ptr_body = true;
    res->ptr_body = ptr;
    res->ptr_body_len = len;
    res->ptr_body_cleanup = cleanup;
    res->ptr_body_cleanup_ctx = ctx;
}

// ... (request parsing omitted) ...

int headers_have_content_length(cwist_http_header_node *headers) {
    cwist_http_header_node *curr = headers;
    while (curr) {
        if (curr->key && curr->key->data && strcasecmp(curr->key->data, "Content-Length") == 0) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

// Helper to serialize headers only
static size_t serialize_headers(cwist_http_response *res, char *buf, size_t buf_size) {
    size_t body_len = 0;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
    } else if (res->body) {
        body_len = res->body->size;
    }
    int offset = 0;
    
    // Status Line
    offset += snprintf(buf + offset, buf_size - offset, "%s %d %s\r\n",
             res->version->data ? res->version->data : "HTTP/1.1",
             res->status_code,
             res->status_text->data ? res->status_text->data : "OK");

    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
             offset += snprintf(buf + offset, buf_size - offset, "%s: %s\r\n", curr->key->data, curr->value->data);
        }
        curr = curr->next;
    }

    if (!headers_have_content_length(res->headers)) {
        offset += snprintf(buf + offset, buf_size - offset, "Content-Length: %zu\r\n", body_len);
    }

    if (!headers_have_connection(res->headers)) {
        if (res->keep_alive) {
            offset += snprintf(buf + offset, buf_size - offset, "Connection: keep-alive\r\n");
        } else {
            offset += snprintf(buf + offset, buf_size - offset, "Connection: close\r\n");
        }
    }

    offset += snprintf(buf + offset, buf_size - offset, "\r\n");
    return offset;
}

#include <sys/uio.h> // For writev and BSD sendfile

static bool cwist_http_stream_file_fast(int client_fd, cwist_http_response *res) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    if (!res || !res->use_file_stream || res->file_stream_fd < 0) return false;
    size_t remaining = res->file_stream_len;
    off_t offset = res->file_stream_offset;
    while (remaining > 0) {
#if defined(__linux__)
        ssize_t sent = sendfile(client_fd, res->file_stream_fd, &offset, remaining);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        if (sent == 0) break;
        remaining -= (size_t)sent;
#elif defined(__APPLE__)
        off_t chunk = (off_t)remaining;
        int rc = sendfile(res->file_stream_fd, client_fd, offset, &chunk, NULL, 0);
        if (rc == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                if (chunk == 0) continue;
                offset += chunk;
                remaining -= (size_t)chunk;
                continue;
            }
            return false;
        }
        if (chunk == 0) break;
        offset += chunk;
        remaining -= (size_t)chunk;
#elif defined(__FreeBSD__)
        off_t sent = 0;
        size_t chunk = remaining;
        int rc = sendfile(res->file_stream_fd, client_fd, offset, chunk, NULL, &sent, 0);
        if (rc == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                if (sent == 0) continue;
                offset += sent;
                remaining -= (size_t)sent;
                continue;
            }
            return false;
        }
        if (sent == 0) break;
        offset += sent;
        remaining -= (size_t)sent;
#endif
    }
    res->file_stream_offset = offset;
    return remaining == 0;
#else
    (void)client_fd;
    (void)res;
    errno = ENOTSUP;
    return false;
#endif
}

cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 1. Prepare Headers (On Stack)
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = serialize_headers(res, header_buf, sizeof(header_buf));

    // 2. Prepare Body
    const void *body_ptr = NULL;
    size_t body_len = 0;

    if (res->is_ptr_body) {
        body_ptr = res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body_ptr = res->body->data;
        body_len = res->body->size;
    }

    // 3. sendmsg (Scatter/Gather + Flags) - Zero Copy Send
    struct msghdr msg = {0};
    struct iovec iov[2];
    int iov_cnt = 1;

    iov[0].iov_base = header_buf;
    iov[0].iov_len = header_len;

    if (!res->use_file_stream && body_len > 0 && body_ptr) {
        iov[1].iov_base = (void*)body_ptr;
        iov[1].iov_len = body_len;
        iov_cnt = 2;
    }

    msg.msg_iov = iov;
    msg.msg_iovlen = iov_cnt;

    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
    #endif

    ssize_t written = sendmsg(client_fd, &msg, flags);
    if (written < 0) {
        err.error.err_i16 = -1;
    } else {
        err.error.err_i16 = 0;
        if (res->use_file_stream) {
            if (!cwist_http_stream_file_fast(client_fd, res)) {
                err.error.err_i16 = -1;
            }
        }
    }

    cwist_http_response_release_ptr_body(res);
    cwist_http_response_release_file_stream(res);
    return err;
}

cwist_sstring *cwist_http_stringify_response(cwist_http_response *res) {
    // Deprecated / Debug only
    if (!res) return NULL;
    cwist_sstring *s = cwist_sstring_create();
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    serialize_headers(res, header_buf, sizeof(header_buf));
    cwist_sstring_assign(s, header_buf);
    if (res->is_ptr_body && res->ptr_body) {
        cwist_sstring_append_len(s, (char*)res->ptr_body, res->ptr_body_len);
    } else if (res->body) {
        cwist_sstring_append(s, res->body->data);
    }
    return s;
}

cwist_http_request *cwist_http_parse_request(const char *raw_request) {
    if (!raw_request) return NULL;

    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    
    const char *line_start = raw_request;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) {
        cwist_http_request_destroy(req);
        return NULL;
    }

    const char *line_end = strstr(line_start, "\r\n");
    if (!line_end || line_end > header_end) { 
        cwist_http_request_destroy(req); 
        return NULL; 
    }

    // 1. Request Line
    int request_line_len = line_end - line_start;
    char *request_line = (char*)malloc(request_line_len + 1);
    if (!request_line) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    strncpy(request_line, line_start, request_line_len);
    request_line[request_line_len] = '\0';
    
    char *next_ptr;
    char *method_str = strtok_r(request_line, " ", &next_ptr);
    char *path_str = strtok_r(NULL, " ", &next_ptr);
    char *version_str = strtok_r(NULL, " ", &next_ptr);
    
    if (method_str) req->method = cwist_http_string_to_method(method_str);
    if (path_str) {
      char *query = strchr(path_str, '?');
      if(query) {
        *query = '\0';
        cwist_sstring_assign(req->path, path_str);
        cwist_sstring_assign(req->query, query + 1); // exclude ? mark
        cwist_query_map_parse(req->query_params, req->query->data);
      } else {
        cwist_sstring_assign(req->path, path_str);
        cwist_sstring_assign(req->query, "");
      }
    }

    if (version_str) {
        cwist_sstring_assign(req->version, version_str);
        if (strcmp(version_str, "HTTP/1.1") == 0) {
            req->keep_alive = true;
        } else {
            req->keep_alive = false;
        }
    }
    
    free(request_line);

    // 2. Headers
    line_start = line_end + 2; // Skip \r\n
    while (line_start < header_end) {
        line_end = strstr(line_start, "\r\n");
        if (!line_end) break;

        if (line_end == line_start) {
            // Empty line found
            line_start += 2;
            break;
        }
        
        int header_len = line_end - line_start;
        char *header_line = (char*)malloc(header_len + 1);
        if (header_line) {
            strncpy(header_line, line_start, header_len);
            header_line[header_len] = '\0';
            
            char *colon = strchr(header_line, ':');
            if (colon) {
                *colon = '\0';
                char *key = header_line;
                char *value = colon + 1;
                while (*value == ' ') value++; // Trim leading space
                
                cwist_http_header_add(&req->headers, key, value);
                if (header_key_is_connection(key)) {
                    if (header_value_is_close(value)) {
                        req->keep_alive = false;
                    } else if (header_value_is_keep_alive(value)) {
                        req->keep_alive = true;
                    }
                } else if (strcasecmp(key, "Content-Length") == 0) {
                    req->content_length = (size_t)atoll(value);
                }
            }
            free(header_line);
        }
        
        line_start = line_end + 2;
    }

    const char *body_start = header_end + 4;
    if (*body_start != '\0') {
        cwist_sstring_assign(req->body, (char*)body_start);
    }

    return req;
}



cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len) {
    size_t total_received = *buf_len;
    char *header_end = NULL;

    // 1. Read until headers are complete
    while (!(header_end = strstr(read_buf, "\r\n\r\n"))) {
        if (total_received >= buf_size - 1) {
            // Buffer full, but headers not complete
            return NULL;
        }
        
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
        if (ret <= 0) return NULL; // Timeout or error

        ssize_t bytes = recv(client_fd, read_buf + total_received, buf_size - 1 - total_received, 0);
        if (bytes <= 0) return NULL;
        total_received += (size_t)bytes;
        read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(read_buf);
    if (!req) return NULL;

    size_t header_len = (header_end + 4) - read_buf;
    size_t body_received = total_received - header_len;

    // 2. Read body based on Content-Length
    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        // Allocate body
        char *body = malloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = (body_received < req->content_length) ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        while (current_body_len < req->content_length) {
            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
            int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
            if (ret <= 0) {
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            ssize_t bytes = recv(client_fd, body + current_body_len, req->content_length - current_body_len, 0);
            if (bytes <= 0) {
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        free(body);

        // Calculate leftovers
        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(read_buf, header_end + 4 + req->content_length, leftover_len);
            *buf_len = leftover_len;
        } else {
            *buf_len = 0;
        }
    } else {
        // No body, leftovers are everything after headers
        if (body_received > 0) {
            memmove(read_buf, header_end + 4, body_received);
            *buf_len = body_received;
        } else {
            *buf_len = 0;
        }
    }
    read_buf[*buf_len] = '\0';

    return req;
}

typedef struct {
    const char *ext;
    const char *mime;
} cwist_mime_entry;

static const cwist_mime_entry CWIST_MIME_TABLE[] = {{".html", "text/html; charset=utf-8"},
                                                    {".htm", "text/html; charset=utf-8"},
                                                    {".css", "text/css; charset=utf-8"},
                                                    {".js", "application/javascript"},
                                                    {".mjs", "application/javascript"},
                                                    {".json", "application/json"},
                                                    {".wasm", "application/wasm"},
                                                    {".png", "image/png"},
                                                    {".jpg", "image/jpeg"},
                                                    {".jpeg", "image/jpeg"},
                                                    {".gif", "image/gif"},
                                                    {".svg", "image/svg+xml"},
                                                    {".txt", "text/plain; charset=utf-8"},
                                                    {".ico", "image/x-icon"}};

/**
 * @brief Guess a MIME type from a filename extension.
 * @return Static MIME string; "application/octet-stream" when unknown.
 */
static const char *cwist_guess_mime(const char *file_path) {
    if (!file_path) return "application/octet-stream";
    const char *dot = strrchr(file_path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    for (size_t i = 0; i < sizeof(CWIST_MIME_TABLE) / sizeof(CWIST_MIME_TABLE[0]); i++) {
        if (strcasecmp(dot, CWIST_MIME_TABLE[i].ext) == 0) {
            return CWIST_MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/**
 * @brief Prepare a response to serve a file either by buffering or direct streaming.
 * @param res Response object to populate.
 * @param file_path Filesystem path to the file that should be served.
 * @param content_type_hint Optional MIME type override.
 * @param out_size Optional output pointer receiving the file size in bytes.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_http_response_send_file(cwist_http_response *res, const char *file_path,
                                            const char *content_type_hint, size_t *out_size) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!res || !file_path) {
        err.error.err_i16 = -EINVAL;
        return err;
    }

    cwist_http_response_release_file_stream(res);
    cwist_http_response_release_ptr_body(res);

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        err.error.err_i16 = -errno;
        return err;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        err.error.err_i16 = -errno;
        close(fd);
        return err;
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        err.error.err_i16 = -EISDIR;
        return err;
    }

    bool endpoint_file = cwist_endpoint_has(res->endpoint_opts, CWIST_ENDPOINT_FILE);

    if (!endpoint_file && (size_t)st.st_size > CWIST_HTTP_MAX_BODY_SIZE) {
        close(fd);
        err.error.err_i16 = -EFBIG;
        return err;
    }

    size_t file_size = (size_t)st.st_size;
    bool use_fast_stream = false;

    if (file_size > 0 && endpoint_file) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        res->use_file_stream = true;
        res->file_stream_fd = fd;
        res->file_stream_len = file_size;
        res->file_stream_offset = 0;
        res->file_stream_auto_close = true;
        use_fast_stream = true;
#endif
    }

    char *buffer = NULL;

    if (!use_fast_stream && file_size > 0) {
        buffer = (char *)cwist_alloc(file_size);
        if (!buffer) {
            close(fd);
            err.error.err_i16 = -ENOMEM;
            return err;
        }
    }

    if (!use_fast_stream) {
        size_t total_read = 0;
        while (total_read < file_size) {
            ssize_t bytes = read(fd, buffer + total_read, file_size - total_read);
            if (bytes < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -errno;
                cwist_free(buffer);
                close(fd);
                return err;
            }
            if (bytes == 0) {
                err.error.err_i16 = -EIO;
                cwist_free(buffer);
                close(fd);
                return err;
            }
            total_read += (size_t)bytes;
        }
        close(fd);

        if (file_size > 0) {
            cwist_sstring_assign_len(res->body, buffer, file_size);
            cwist_free(buffer);
        } else {
            cwist_sstring_assign(res->body, "");
        }
    } else {
        cwist_sstring_borrow(res->body, "", 0);
    }

    const char *mime = content_type_hint ? content_type_hint : cwist_guess_mime(file_path);
    if (mime && !cwist_http_header_get(res->headers, "Content-Type")) {
        cwist_http_header_add(&res->headers, "Content-Type", mime);
    }

    if (out_size) {
        *out_size = file_size;
    }

    res->status_code = CWIST_HTTP_OK;
    err.error.err_i16 = 0;
    return err;
}

/* --- Predefined Static Blobs --- */

const char CWIST_BLOB_200_OK[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
const char CWIST_BLOB_404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\n404 Not Found";
const char CWIST_BLOB_500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 21\r\nConnection: close\r\n\r\nInternal Server Error";

/* --- Socket Manipulation --- */
/* Everything from here on is the socket server runtime: WASI preview1 has
 * no sockets, so the whole section compiles out there. WASM hosts drive
 * requests through cwist_app_dispatch_memory() instead. */
#ifndef CWIST_WASI_NO_SOCKETS

/**
 * @brief Create, configure, bind, and listen on an IPv4 TCP socket.
 * @param sockv4 Output sockaddr structure populated for the bind call.
 * @param address IPv4 address string to bind.
 * @param port TCP port to listen on.
 * @param backlog Listen backlog passed to listen(2).
 * @return Listening socket fd on success, or a negative CWIST socket error code.
 */
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port,
                           uint16_t backlog) {
    int server_fd = -1;
    int opt = 1;

    if (!address || inet_pton(AF_INET, address, &sockv4->sin_addr) != 1) {
        return CWIST_HTTP_UNAVAILABLE_ADDRESS;
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        cJSON *err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err_json, "err", "Failed to create IPv4 socket");
        char *cjson_error_log = cJSON_Print(err_json);
        perror(cjson_error_log);
        cwist_free(cjson_error_log);
        cJSON_Delete(err_json);

        return CWIST_CREATE_SOCKET_FAILED;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        cJSON *err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err_json, "err", "Failed to set up IPv4 socket options");
        char *cjson_error_log = cJSON_Print(err_json);
        perror(cjson_error_log);
        cwist_free(cjson_error_log);
        cJSON_Delete(err_json);

        return CWIST_HTTP_SETSOCKOPT_FAILED;
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
  int no_sig_pipe = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sig_pipe, sizeof(no_sig_pipe));
#endif

  sockv4->sin_family = AF_INET;

  sockv4->sin_addr.s_addr = addr;
  sockv4->sin_port = htons(port);

#if defined(__APPLE__) || defined(__FreeBSD__)
#ifdef SO_NOSIGPIPE
    int no_sig_pipe = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sig_pipe, sizeof(no_sig_pipe));
#endif
#endif

    sockv4->sin_family = AF_INET;
    sockv4->sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)sockv4, sizeof(struct sockaddr_in)) < 0) {
        cJSON *err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err_json, "err", "Failed to bind IPv4 socket");
        char *cjson_error_log = cJSON_Print(err_json);
        perror(cjson_error_log);
        cwist_free(cjson_error_log);
        cJSON_Delete(err_json);

        return CWIST_HTTP_BIND_FAILED;
    }

    if (listen(server_fd, backlog) < 0) {
        cJSON *err_json = cJSON_CreateObject();
        char err_msg[128];
        char err_format[128] = "Failed to listen at %s:%d";
        snprintf(err_msg, 127, err_format, address, port);

        cJSON_AddStringToObject(err_json, "err", err_msg);
        char *cjson_error_log = cJSON_Print(err_json);
        perror(cjson_error_log);
        cwist_free(cjson_error_log);
        cJSON_Delete(err_json);

        return CWIST_HTTP_LISTEN_FAILED;
    }

    return server_fd;
}

struct thread_payload {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
};

static void *thread_handler(void *arg) {
    struct thread_payload *payload = (struct thread_payload *)arg;
    int client_fd = payload->client_fd;
    void (*handler_func)(int, void *) = payload->handler_func;
    void *ctx = payload->ctx;
    free(payload);
    handler_func(client_fd, ctx);
    return NULL;
}

/**
 * @brief Apply a small sleep when repeated accept failures suggest resource pressure.
 * @param err errno value returned by accept(2).
 */
static void cwist_accept_error_backoff(int err) {
    switch (err) {
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM: {
            fprintf(stderr, "[CWIST] accept() backoff triggered: %s\n", strerror(err));
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000; // 50ms
            nanosleep(&ts, NULL);
            break;
        }
        default: break;
    }
}

/**
 * @brief Service one accepted client in a forked child process.
 * @param client_fd Accepted client socket descriptor.
 * @param handler_func Request handler callback.
 * @param ctx Opaque callback context.
 */
#if !defined(__wasi__)
static void handle_client_forking(int client_fd, void (*handler_func)(int, void *), void *ctx) {
    pid_t pid = fork();
    if (pid == 0) {
        handler_func(client_fd, ctx);
        close(client_fd);
        _exit(0);
    } else if (pid > 0) {
        close(client_fd);
    }
}
#endif /* __wasi__ (no fork) */

/**
 * @brief Accept one client connection and dispatch it according to the current server strategy.
 * @param server_fd Listening server socket.
 * @param sockv4 Scratch sockaddr buffer for accept(2).
 * @param handler_func Callback that handles one accepted client.
 * @param ctx Opaque callback context.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4,
                                  void (*handler_func)(int client_fd, void *), void *ctx) {
    int client_fd = -1;
    struct sockaddr_in peer_addr;
    socklen_t addrlen = sizeof(peer_addr);

    while (true) {
        if ((client_fd = accept(server_fd, (struct sockaddr *)&peer_addr, &addrlen)) < 0) {
            if (errno == EINTR) continue;
            // ... (error handling)
            if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
                fprintf(stderr, "Fatal socket error %d. Exiting accept loop.\n", errno);
                break;
            }
            continue;
        }

        if (sockv4) {
            memcpy(sockv4, &peer_addr, sizeof(peer_addr));
        }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        handler_func(client_fd, ctx);
    }

    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

/**
 * @brief Run the main HTTP accept loop using the configured concurrency strategy.
 * @param server_fd Listening server socket.
 * @param config Server concurrency configuration flags.
 * @param handler Callback that handles one accepted client.
 * @param ctx Opaque callback context.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config,
                                     void (*handler)(int, void *), void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!config || server_fd < 0 || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

#if !defined(__wasi__)
    if (config->use_forking) {
        while (atomic_load(&g_cwist_running)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                int accept_err = errno;
                if (accept_err == EBADF || accept_err == EINVAL) break;
                if (cwist_accept_error_should_retry(accept_err)) {
                    cwist_accept_error_backoff(accept_err);
                    continue;
                }
                err.error.err_i16 = -1;
                return err;
            }
            int nodelay = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            handle_client_forking(client_fd, handler, ctx);
        }
    }
#endif /* __wasi__ (no fork) */

    if (config->use_threading) {
        /* This loop submits to the classic dynamic pool, so initialise that
         * pool whatever CWIST_C1M_MODE says (cwist_app_listen_ex() may have
         * overridden it). */
        if (cwist_http_pool_init_mode(false) != 0) {
            err.error.err_i16 = -1;
            return err;
        }
        while (atomic_load(&g_cwist_running)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -1;
                return err;
            }
            pthread_t thread;
            struct thread_payload *payload = malloc(sizeof(*payload));
            if (!payload) {
                close(client_fd);
                continue;
            }
            payload->client_fd = client_fd;
            payload->handler_func = handler;
            payload->ctx = ctx;
            if (pthread_create(&thread, NULL, thread_handler, payload) == 0) {
                pthread_detach(thread);
            } else {
                free(payload);
                close(client_fd);
            }
        }
        cwist_http_pool_destroy();
        err.error.err_i16 = 0;
        return err;
    }

#ifdef __linux__
    if (config->use_epoll) {
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
            close(epoll_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (atomic_load(&g_cwist_running)) {
            struct epoll_event events[1024];
            int count = epoll_wait(epoll_fd, events, 1024, -1);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EBADF) break;
                break;
            }
            for (int i = 0; i < count; i++) {
                if (events[i].data.fd == server_fd) {
                    while (1) {
                        int client_fd = accept(server_fd, NULL, NULL);
                        if (client_fd >= 0) {
                            int nodelay = 1;
                            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                                       sizeof(nodelay));
                            handler(client_fd, ctx);
                        } else {
                            int accept_err = errno;
                            if (accept_err == EAGAIN || accept_err == EWOULDBLOCK) break;
                            if (accept_err == EBADF || accept_err == EINVAL) goto epoll_exit;
                            if (accept_err == EINTR) continue;
                            if (cwist_accept_error_should_retry(accept_err)) {
                                cwist_accept_error_backoff(accept_err);
                                continue;
                            }
                            err.error.err_i16 = -1;
                            close(epoll_fd);
                            return err;
                        }
                    }
                }
            }
        }
    epoll_exit:
        close(epoll_fd);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (config->use_epoll) {
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int kqueue_fd = kqueue();
        if (kqueue_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct kevent change;
        EV_SET(&change, server_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(kqueue_fd, &change, 1, NULL, 0, NULL) < 0) {
            close(kqueue_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (atomic_load(&g_cwist_running)) {
            struct kevent events[16];
            int count = kevent(kqueue_fd, NULL, 0, events, 16, NULL);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EBADF) break;
                break;
            }
            for (int i = 0; i < count; i++) {
                if ((int)events[i].ident == server_fd) {
                    while (1) {
                        int client_fd = accept(server_fd, NULL, NULL);
                        if (client_fd >= 0) {
                            int nodelay = 1;
                            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                                       sizeof(nodelay));
                            handler(client_fd, ctx);
                        } else {
                            int accept_err = errno;
                            if (accept_err == EAGAIN || accept_err == EWOULDBLOCK) break;
                            if (accept_err == EBADF || accept_err == EINVAL) goto kq_exit;
                            if (accept_err == EINTR) continue;
                            if (cwist_accept_error_should_retry(accept_err)) {
                                cwist_accept_error_backoff(accept_err);
                                continue;
                            }
                            err.error.err_i16 = -1;
                            close(kqueue_fd);
                            return err;
                        }
                    }
                }
            }
        }
    kq_exit:
        close(kqueue_fd);
    }
#endif

    return cwist_accept_socket(server_fd, NULL, handler, ctx);
}
#endif /* __wasi__ (socket server runtime) */
