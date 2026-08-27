#define _POSIX_C_SOURCE 200809L
#endif
#include <cwist/net/http/http.h>
#include <cwist/net/http/session.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/arena.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/core/log.h>
#include <cwist/sys/metrics/metrics.h>
#include <ttak/mols_control.h>
#include <ttak/net/lattice.h>
#include <ttak/priority/scheduler.h>
#include "simd_parser.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
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

long get_cpu_cores(void) {
#if defined(_WIN32) || defined(_WIN64)
    /* Windows Environment */
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (long)sysinfo.dwNumberOfProcessors;

#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    /* BSD Variant - Query kernel MIB tree directly via sysctl */
    int mib[2];
    int nproc = 0;
    size_t len = sizeof(nproc);

    mib[0] = CTL_HW;
#if defined(HW_NCPUONLINE)
    /* OpenBSD/FreeBSD preferred: returns counts of actual online cores */
    mib[1] = HW_NCPUONLINE;
#else
    /* Fallback for older BSD kernels */
    mib[1] = HW_NCPU;
#endif

    if (sysctl(mib, 2, &nproc, &len, NULL, 0) == 0) {
        return (long)nproc;
    }
    return 4;

#elif defined(_SC_NPROCESSORS_ONLN)
    /* Linux / Unix POSIX standard */
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    return (nproc >= 4) ? nproc : 4;

#else
    /* Fallback value for undetermined architecture */
    return 4;
#endif
}

long get_optimal_thread_count(void) {
    const char *env = getenv("CWIST_WORKER_THREADS");
    if (env && env[0]) {
        long override = atol(env);
        if (override > 0) return override;
    }
    long cores = get_cpu_cores();
    long count = cores;
    if (count < 4) count = 4;
    if (count > 32) count = 32;
    return count;
}

#define HTTP_TASKS_PER_THREAD 32768

typedef struct {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
} http_pool_task_t;

typedef struct {
    pthread_t thread;
    http_pool_task_t *queue;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int shutdown;
    uint32_t worker_id;
} http_thread_worker_t;

static size_t g_rr_index = 0;
static long g_http_thread_count;
static http_thread_worker_t *g_workers = NULL;

/* Saturation backpressure: track in-flight connections and shed load once
 * every worker thread could be parked on a connection many times over.
 * Past the limit there is no throughput left to win - new arrivals would
 * only inflate tail latency for traffic already being served. Shedding is
 * a single fixed 503 write + close, so it adds no latency to others. */
static _Atomic long g_http_inflight = 0;
#define CWIST_HTTP_INFLIGHT_PER_THREAD 32

static const char CWIST_HTTP_503[] =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

typedef struct {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
    cwist_reactor_t *reactor;
} http_conn_ctx_t;

/* The reactor copies this struct into its pooled, zeroed slot at add time,
 * so the callback reads the slot's inline storage - no per-connection
 * heap allocation, nothing to free here. */
static void http_conn_event_cb(int fd, void *ctx) {
    http_conn_ctx_t *c = (http_conn_ctx_t *)ctx;
    void (*handler)(int, void *) = c->handler_func;
    void *user_ctx = c->ctx;

    handler(fd, user_ctx);
    atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
}

static _Thread_local http_thread_worker_t *t_current_worker = NULL;

static void *http_pool_worker(void *arg) {
    http_thread_worker_t *w = (http_thread_worker_t *)arg;
    ttak_net_lattice_set_worker_id(w->worker_id);

    http_pool_task_t batch[16];

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

static void *http_dynamic_worker_thread(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_dyn_pool.running, memory_order_acquire)) {
        http_pool_task_t *task = NULL;

        pthread_mutex_lock(&g_dyn_pool.lock);
        while (atomic_load_explicit(&g_dyn_pool.running, memory_order_acquire) && !g_dyn_pool.head) {
            atomic_fetch_add_explicit(&g_dyn_pool.idle_workers, 1, memory_order_relaxed);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2; /* 2 second idle timeout */
            int rc = pthread_cond_timedwait(&g_dyn_pool.cond, &g_dyn_pool.lock, &ts);
            atomic_fetch_sub_explicit(&g_dyn_pool.idle_workers, 1, memory_order_relaxed);
            if (rc == ETIMEDOUT && !g_dyn_pool.head) {
                /* Scale down if idle and above base worker threshold */
                long current = atomic_load_explicit(&g_dyn_pool.active_workers, memory_order_relaxed);
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

static bool http_spawn_worker(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    atomic_fetch_add_explicit(&g_dyn_pool.active_workers, 1, memory_order_relaxed);
    int rc = pthread_create(&tid, &attr, http_dynamic_worker_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_fetch_sub_explicit(&g_dyn_pool.active_workers, 1, memory_order_relaxed);
        return false;
    }
    return true;
}

int cwist_http_pool_init(void) {
    const char *c1m = getenv("CWIST_C1M_MODE");
    bool use_c1m = true;
    if (c1m) {
        if (c1m[0] == '0' || strcmp(c1m, "false") == 0) {
            use_c1m = false;
        }
    }

    g_http_thread_count = get_optimal_thread_count();

    if (use_c1m) {
        /* Pre-allocate reactor workers for async path (CWIST_C1M_MODE=1) */
        g_workers = cwist_alloc(g_http_thread_count * sizeof(http_thread_worker_t));
        if (!g_workers) return -1;
        g_rr_index = 0;
        memset(g_workers, 0, g_http_thread_count * sizeof(http_thread_worker_t));

        for (int i = 0; i < g_http_thread_count; i++) {
            g_workers[i].reactor = cwist_reactor_create();
            if (!g_workers[i].reactor) return -1;
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

        /* Pre-warm core worker threads */
        for (int i = 0; i < g_http_thread_count; i++) {
            if (!http_spawn_worker()) {
                return -1;
            }
        }
    }
    return 0;
}

void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx) {
    /* Backpressure gate: shed before doing any routing/allocation work when
     * saturated. In-flight = queued + actively served connections. */
    long limit = g_http_thread_count * CWIST_HTTP_INFLIGHT_PER_THREAD;
    long inflight = atomic_fetch_add_explicit(&g_http_inflight, 1, memory_order_acq_rel) + 1;
    if (inflight > limit) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        send(client_fd, CWIST_HTTP_503, sizeof(CWIST_HTTP_503) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        close(client_fd);
        return;
    }

    /* Deterministic worker selection using Choi Seok-jeong's MOLS to minimize cache bouncing. */
    uint16_t node_id = (uint16_t)(client_fd % TTAK_MOLS_NODE_COUNT);
    uint32_t mixed = ttak_apply_mols_control(node_id, (uint32_t)g_rr_index);
    size_t worker_idx = mixed % (size_t)g_http_thread_count;

    g_rr_index = (g_rr_index + 1) % (size_t)g_http_thread_count;

    http_thread_worker_t *w = &g_workers[worker_idx];

    http_conn_ctx_t c = {
        .client_fd = client_fd,
        .handler_func = handler,
        .ctx = ctx,
        .reactor = w->reactor,
    };

    if (!cwist_reactor_add(w->reactor, client_fd, http_conn_event_cb, &c, sizeof(c))) {
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
    atomic_fetch_add_explicit(&g_dyn_pool.pending_tasks, 1, memory_order_release);
    pthread_cond_signal(&g_dyn_pool.cond);
    pthread_mutex_unlock(&g_dyn_pool.lock);

    long current = atomic_load_explicit(&g_dyn_pool.active_workers, memory_order_relaxed);
    long idle = atomic_load_explicit(&g_dyn_pool.idle_workers, memory_order_relaxed);
    long max_w = atomic_load_explicit(&g_dyn_pool.max_workers, memory_order_relaxed);
    if (idle == 0 && current < max_w) {
        http_spawn_worker();
    }
}

bool cwist_http_pool_rearm_current(int client_fd, void (*handler)(int, void *), void *ctx) {
    if (!t_current_worker || !t_current_worker->reactor || client_fd < 0) return false;

    http_conn_ctx_t c = {
        .client_fd = client_fd,
        .handler_func = handler,
        .ctx = ctx,
        .reactor = t_current_worker->reactor,
    };

    return cwist_reactor_add(t_current_worker->reactor, client_fd, http_conn_event_cb, &c, sizeof(c));
}

void cwist_http_pool_destroy(void) {
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

    if (g_workers) {
        for (int i = 0; i < g_http_thread_count; i++) {
            if (g_workers[i].reactor) {
                cwist_reactor_stop(g_workers[i].reactor);
            }
        }
        for (int i = 0; i < g_http_thread_count; i++) {
            pthread_join(g_workers[i].thread, NULL);
        }
#else
        pthread_join(g_workers[i].thread, NULL);
#endif
        pthread_mutex_destroy(&g_workers[i].mutex);
        pthread_cond_destroy(&g_workers[i].cond_not_empty);
        pthread_cond_destroy(&g_workers[i].cond_not_full);
        if (g_workers[i].queue) {
            cwist_free(g_workers[i].queue);
        }
    }

    cwist_free(g_workers);
    g_workers = nullptr;
    g_http_thread_count = 0;
}
/* --- End Thread Pool --- */

/**
 * @file http.c
 * @brief Core HTTP request/response allocation, serialization, socket, and server-loop helpers.
 */

const int CWIST_CREATE_SOCKET_FAILED     = -1;
const int CWIST_HTTP_UNAVAILABLE_ADDRESS = -2;
const int CWIST_HTTP_BIND_FAILED         = -3;
const int CWIST_HTTP_SETSOCKOPT_FAILED   = -4;
const int CWIST_HTTP_LISTEN_FAILED       = -5;

/* --- Helpers --- */

const char *cwist_http_method_to_string(cwist_http_method_t method) {
    switch (method) {
        case CWIST_HTTP_GET: return "GET";
        case CWIST_HTTP_POST: return "POST";
        case CWIST_HTTP_PUT: return "PUT";
        case CWIST_HTTP_DELETE: return "DELETE";
        case CWIST_HTTP_PATCH: return "PATCH";
        case CWIST_HTTP_HEAD: return "HEAD";
        case CWIST_HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (strcmp(method_str, "GET") == 0) return CWIST_HTTP_GET;
    if (strcmp(method_str, "POST") == 0) return CWIST_HTTP_POST;
    if (strcmp(method_str, "PUT") == 0) return CWIST_HTTP_PUT;
    if (strcmp(method_str, "DELETE") == 0) return CWIST_HTTP_DELETE;
    if (strcmp(method_str, "PATCH") == 0) return CWIST_HTTP_PATCH;
    if (strcmp(method_str, "HEAD") == 0) return CWIST_HTTP_HEAD;
    if (strcmp(method_str, "OPTIONS") == 0) return CWIST_HTTP_OPTIONS;
    return CWIST_HTTP_UNKNOWN;
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
static cwist_error_t cwist_http_sstring_assign_arena(cwist_sstring *str, cwist_arena_t *arena, const char *data, size_t len) {
    if (arena) {
        char *buf = (char *)cwist_arena_alloc(arena, len + 1);
        if (buf) {
            if (str->data && !str->borrows_buffer) {
                cwist_free(str->data);
            }
            if (data && len > 0) memcpy(buf, data, len);
            buf[len] = '\0';
            str->data = buf;
            str->size = len;
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
static cwist_error_t cwist_http_header_add_ex_len(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, size_t key_len, const char *value, size_t value_len) {
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

    cwist_http_sstring_assign_arena(node->key, arena, key, key_len);
    cwist_http_sstring_assign_arena(node->value, arena, value, value_len);

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
static cwist_error_t cwist_http_header_add_ex(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, const char *value) {
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

    cwist_http_sstring_assign_arena(node->key, arena, key, key ? strlen(key) : 0);
    cwist_http_sstring_assign_arena(node->value, arena, value, value ? strlen(value) : 0);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend a header whose key/value live in static storage (zero-copy).
 *
 * Used for compile-time constant headers such as the default security set:
 * the node and sstring structs come from the arena and the character bytes
 * are borrowed, so a default response pays no heap traffic for headers at
 * all. Key and value must outlive the header list.
 */
static cwist_error_t cwist_http_header_add_static(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, const char *value) {
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
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header strings");
        return err;
    }

    cwist_sstring_borrow(node->key, key, strlen(key));
    cwist_sstring_borrow(node->value, value, strlen(value));

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend one header node to the linked-list header collection.
 * @param head Header-list head pointer to update.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error describing success or allocation failure.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    cwist_http_header_node *node = (cwist_http_header_node *)malloc(sizeof(cwist_http_header_node));
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }

    node->key = smartstring_create();
    node->value = smartstring_create();
    node->next = NULL;

    smartstring_assign(node->key, (char *)key);
    smartstring_assign(node->value, (char *)value);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    cwist_http_header_node *curr = head;
    while (curr) {
        // case-insensitive comparison for headers is standard, but keeping it strict for now for simplicity
        if (curr->key->data && strcmp(curr->key->data, key) == 0) {
            return curr->value->data;
        }
        curr = curr->next;
    }
    return NULL;
}

/**
 * @brief Add default security headers to an HTTP response if not already present.
 * @param res Response object to populate.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res) {
    if (!res) return;

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
        cwist_http_header_add_static(&res->headers, arena, "Referrer-Policy", "strict-origin-when-cross-origin");
    }
    if (!cwist_http_header_get(res->headers, "Content-Security-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Content-Security-Policy",
            "default-src 'self'; "
            "script-src 'self' 'unsafe-inline' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; "
            "font-src 'self' https://fonts.gstatic.com; "
            "img-src 'self' data:; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self'; "
            "object-src 'none';");
    }
    if (!cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Cross-Origin-Resource-Policy", "same-origin");
    }
    if (!cwist_http_header_get(res->headers, "Strict-Transport-Security")) {
        cwist_http_header_add_static(&res->headers, arena, "Strict-Transport-Security", "max-age=31536000; includeSubDomains");
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
        smartstring_destroy(curr->key);
        smartstring_destroy(curr->value);
        free(curr);
        curr = next;
    }
}

/**
 * @brief Check whether a header key names the Connection header.
 * @param key Header key to inspect.
 * @return true when the key is "connection" ignoring case.
 */
static bool header_key_is_connection(const char *key) {
    if (!key) return false;
    return strcasecmp(key, "connection") == 0;
}


/**
 * @brief Detect whether the current header list already contains a Connection header.
 * @param head Head of the header linked list.
 * @return true when a Connection header is present.
 */
static bool headers_have_connection(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        if (curr->key && curr->key->data && header_key_is_connection(curr->key->data)) {
            return true;
        }
        curr = curr->next;
    }
    return false;
}

/* --- Request Lifecycle --- */

cwist_http_request *cwist_http_request_create(void) {
    cwist_http_request *req = (cwist_http_request *)malloc(sizeof(cwist_http_request));
    if (!req) return NULL;

    req->method = CWIST_HTTP_GET; // Default
    req->path = cwist_sstring_create();
    req->query = cwist_sstring_create();
    req->query_params = cwist_query_map_create();
    req->path_params = cwist_query_map_create();
    req->version = cwist_sstring_create();
    req->headers = NULL;
    req->body = cwist_sstring_create();
    req->keep_alive = true;
    req->client_fd = -1;
    req->app = NULL;
    req->db = NULL;
    req->upgraded = false;
    req->content_length = 0;
    req->stream_id = 0;
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

cwist_sstring* cwist_get_client_ip_from_fd(int fd) {
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "127.0.0.1");
    // First, check if fd is available
    // If unavailable, return localhost
    if(fd <= 0) return s;

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    // get client info
    if(getpeername(fd, (struct sockaddr *)&addr, &len) == -1) {
        fprintf(stdout, "[ERROR] Failed to get client info from file descriptor");
        return s;
    }

    char ip[INET6_ADDRSTRLEN];

    if(addr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
    } else if(addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
    }

    // assign found ip as a value
    cwist_sstring_assign(s, ip);
    return s;
}

void cwist_http_request_destroy(cwist_http_request *req) {
    if (req) {
        cwist_sstring_destroy(req->path);
        cwist_sstring_destroy(req->query);
        cwist_query_map_destroy(req->query_params);
        cwist_query_map_destroy(req->path_params);
        cwist_sstring_destroy(req->version);
        cwist_sstring_destroy(req->body);
        cwist_http_header_free_all(req->headers);
        free(req);
    }
}

/* --- Response Lifecycle --- */

cwist_http_response *cwist_http_response_create(void) {
    cwist_http_response *res = (cwist_http_response *)malloc(sizeof(cwist_http_response));
    if (!res) return NULL;

    res->version = smartstring_create();
    res->status_code = CWIST_HTTP_OK;
    res->status_text = smartstring_create();
    res->headers = NULL;
    res->body = smartstring_create();

    // Defaults (borrowed statics; handlers may overwrite via regular assign)
    cwist_sstring_borrow(res->version, "HTTP/1.1", 8);
    cwist_sstring_borrow(res->status_text, "OK", 2);

    cwist_http_response_add_security_headers(res);

    return res;
}

void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        smartstring_destroy(res->version);
        smartstring_destroy(res->status_text);
        smartstring_destroy(res->body);
        cwist_http_header_free_all(res->headers);
        free(res);
    }
}

/**
 * @brief Attach an unmanaged zero-copy body pointer to a response.
 * @param res Response object to modify.
 * @param ptr External body pointer.
 * @param len Length of the external body in bytes.
 */
void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len) {
    cwist_http_response_set_body_ptr_managed(res, ptr, len, NULL, NULL);
}

/**
 * @brief Attach a managed zero-copy body pointer and optional cleanup hook to a response.
 * @param res Response object to modify.
 * @param ptr External body pointer.
 * @param len Length of the external body in bytes.
 * @param cleanup Optional cleanup callback for the body pointer.
 * @param ctx Opaque context forwarded to the cleanup callback.
 */
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

/**
 * @brief Set the Alt-Svc header value for HTTP/3 upgrade advertisement.
 * @param res Response object to modify.
 * @param alt_svc Alt-Svc header value (e.g., `h3=":443"; ma=86400`).
 *        Pass NULL to clear any previously set value.
 */
void cwist_http_response_set_alt_svc(cwist_http_response *res, const char *alt_svc) {
    if (!res) return;
    if (res->alt_svc) {
        cwist_free(res->alt_svc);
        res->alt_svc = NULL;
    }
    if (alt_svc) {
        res->alt_svc = strdup(alt_svc);
    }
}

// ... (request parsing omitted) ...

/**
 * @brief Detect whether a header list already defines Content-Length.
 * @param headers Header linked list to scan.
 * @return 1 when a Content-Length header is present, otherwise 0.
 */
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

/**
 * @brief Serialize the HTTP status line and headers into a caller-provided buffer.
 * @param res Response object to serialize.
 * @param buf Destination buffer for the header block.
 * @param buf_size Total capacity of @p buf in bytes.
 * @return Number of bytes written into the buffer.
 */
static size_t serialize_headers(cwist_http_response *res, char *buf, size_t buf_size) {
    size_t body_len = 0;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
    } else if (res->body) {
        body_len = res->body->size;
    }
    size_t offset = 0;
    
    // Status Line
    const char *status_txt = (res->status_text && res->status_text->data) 
                             ? res->status_text->data 
                             : "OK";
    if (offset < buf_size) {
        int n = snprintf(buf + offset, buf_size - offset, "%s %d %s\r\n",
                 res->version->data ? res->version->data : "HTTP/1.1",
                 res->status_code,
                 status_txt);
        if (n > 0) {
            offset += n;
            if (offset > buf_size) offset = buf_size;
        }
    }

    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
            if (offset < buf_size) {
                int n = snprintf(buf + offset, buf_size - offset, "%s: %s\r\n", curr->key->data, curr->value->data);
                if (n > 0) {
                    offset += n;
                    if (offset > buf_size) offset = buf_size;
                }
            }
        }
        curr = curr->next;
    }

    if (!headers_have_content_length(res->headers)) {
        if (offset < buf_size) {
            int n = snprintf(buf + offset, buf_size - offset, "Content-Length: %zu\r\n", body_len);
            if (n > 0) {
                offset += n;
                if (offset > buf_size) offset = buf_size;
            }
        }
    }

    if (!headers_have_connection(res->headers)) {
        if (offset < buf_size) {
            int n = snprintf(buf + offset, buf_size - offset, "Connection: %s\r\n", res->keep_alive ? "keep-alive" : "close");
            if (n > 0) {
                offset += n;
                if (offset > buf_size) offset = buf_size;
            }
        }
    }

    if (res->alt_svc) {
        if (offset < buf_size) {
            int n = snprintf(buf + offset, buf_size - offset, "Alt-Svc: %s\r\n", res->alt_svc);
            if (n > 0) {
                offset += n;
                if (offset > buf_size) offset = buf_size;
            }
        }
    }

    if (offset < buf_size) {
        int n = snprintf(buf + offset, buf_size - offset, "\r\n");
        if (n > 0) {
            offset += n;
            if (offset > buf_size) offset = buf_size;
        }
    }
    return offset;
}

/**
 * @brief Serialize the status line and headers into a caller-provided buffer.
 *
 * Non-static wrapper around serialize_headers() so the TLS send path can
 * stream headers and body separately instead of materializing one blob.
 *
 * @param res Response object to serialize.
 * @param buf Destination buffer for the header block.
 * @param buf_size Total capacity of @p buf in bytes.
 * @return Number of bytes written into the buffer.
 */
size_t cwist_http_serialize_headers(cwist_http_response *res, char *buf, size_t buf_size) {
    return serialize_headers(res, buf, buf_size);
}

#include <sys/uio.h> // For writev and BSD sendfile

/**
 * @brief Send an entire iovec over a (possibly non-blocking) socket.
 * Handles EINTR, EAGAIN/EWOULDBLOCK with POLLOUT polling, and partial writes.
 * @return 0 on success, -1 on fatal error or timeout.
 */
static int cwist_http_sendmsg_all(int fd, struct iovec *iov, int iovcnt, int flags) {
    struct iovec *cur = iov;
    int curcnt = iovcnt;
    while (curcnt > 0) {
        struct msghdr msg = {0};
        msg.msg_iov = cur;
        msg.msg_iovlen = (size_t)curcnt;
        ssize_t n = sendmsg(fd, &msg, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0) return -1;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;

        while (curcnt > 0 && (size_t)n >= cur->iov_len) {
            n -= (ssize_t)cur->iov_len;
            cur++;
            curcnt--;
        }
        if (curcnt > 0) {
            cur->iov_base = (char *)cur->iov_base + n;
            cur->iov_len -= (size_t)n;
        }
    }
    return 0;
}

/**
 * @brief Attempt an optimized file-stream send path using platform sendfile support.
 * @param client_fd Connected client socket descriptor.
 * @param res Response object configured for file streaming.
 * @return true when the file body and headers were transmitted successfully.
 */
static bool cwist_http_stream_file_fast(int client_fd, cwist_http_response *res) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    if (!res || !res->use_file_stream || res->file_stream_fd < 0) return false;
    size_t remaining = res->file_stream_len;
    off_t offset = res->file_stream_offset;
    while (remaining > 0) {
#if defined(__linux__)
        ssize_t sent = sendfile(client_fd, res->file_stream_fd, &offset, remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
                continue;
            }
            return false;
        }
        if (sent == 0) break;
        remaining -= (size_t)sent;
#elif defined(__APPLE__)
        off_t chunk = (off_t)remaining;
        int rc = sendfile(res->file_stream_fd, client_fd, offset, &chunk, NULL, 0);
        if (rc == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (chunk > 0) {
                    offset += chunk;
                    remaining -= (size_t)chunk;
                }
                struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
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
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sent > 0) {
                    offset += sent;
                    remaining -= (size_t)sent;
                }
                struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
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

/**
 * @brief Serialize and send an HTTP response to a connected client socket.
 * @param client_fd Connected client socket descriptor.
 * @param res Response object to send.
 * @return Tagged CWIST error describing success or transmission failure.
 */
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
    struct iovec iov[2];
    int iov_cnt = 1;

    iov[0].iov_base = header_buf;
    iov[0].iov_len = header_len;

    if (!res->use_file_stream && body_len > 0 && body_ptr) {
        iov[1].iov_base = (void*)body_ptr;
        iov[1].iov_len = body_len;
        iov_cnt = 2;
    }

    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
    #endif

    if (cwist_http_sendmsg_all(client_fd, iov, iov_cnt, flags) != 0) {
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

/**
 * @brief Send only the status line and headers of a response (RFC 9110 §9.3.2
 * HEAD semantics): Content-Length reflects the would-be body, but no body
 * bytes are written. Body resources are released as in the full send path.
 */
cwist_error_t cwist_http_send_response_head(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = serialize_headers(res, header_buf, sizeof(header_buf));

    struct iovec iov = { .iov_base = header_buf, .iov_len = header_len };
    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    #if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
    #endif

    err.error.err_i16 = (cwist_http_sendmsg_all(client_fd, &iov, 1, flags) == 0) ? 0 : -1;

    cwist_http_response_release_ptr_body(res);
    cwist_http_response_release_file_stream(res);
    return err;
}

/**
 * @brief Send only the status line and headers of a response (RFC 9110 §9.3.2
 * HEAD semantics): Content-Length reflects the would-be body, but no body
 * bytes are written. Body resources are released as in the full send path.
 */
cwist_error_t cwist_http_send_response_head(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = serialize_headers(res, header_buf, sizeof(header_buf));

    struct iovec iov = { .iov_base = header_buf, .iov_len = header_len };
    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    #if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
    #endif

    err.error.err_i16 = (cwist_http_sendmsg_all(client_fd, &iov, 1, flags) == 0) ? 0 : -1;

    cwist_http_response_release_ptr_body(res);
    cwist_http_response_release_file_stream(res);
    return err;
}

/**
 * @brief Send a minimal error response with Connection: close, used to answer
 * malformed requests (400/413/417/431/501) before the connection is dropped.
 * @param fd Connected client socket descriptor.
 * @param status HTTP status code (reason phrase is derived from it).
 * @param msg Plain-text body; NULL falls back to the reason phrase.
 */
void cwist_http_send_error_response(int fd, int status, const char *msg) {
    if (fd < 0) return;

    const char *reason;
    switch (status) {
        case 400: reason = "Bad Request"; break;
        case 413: reason = "Content Too Large"; break;
        case 417: reason = "Expectation Failed"; break;
        case 431: reason = "Request Header Fields Too Large"; break;
        case 500: reason = "Internal Server Error"; break;
        case 501: reason = "Not Implemented"; break;
        case 503: reason = "Service Unavailable"; break;
        default:  reason = "Error"; break;
    }
    if (!msg) msg = reason;

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     status, reason, strlen(msg), msg);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

    struct iovec iov = { .iov_base = buf, .iov_len = len };
    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    #if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
    #endif
    (void)cwist_http_sendmsg_all(fd, &iov, 1, flags);
}

/**
 * @brief Emit the interim 100 Continue response (RFC 9110 §10.1.1) before the
 * request body is read. Best effort: a failed write surfaces on the next recv.
 */
static void http_send_100_continue(int fd) {
    static const char k_continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    struct iovec iov = { .iov_base = (void *)k_continue, .iov_len = sizeof(k_continue) - 1 };
    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    (void)cwist_http_sendmsg_all(fd, &iov, 1, flags);
}

/**
 * @brief Materialize an HTTP response into a contiguous string for debugging or TLS writes.
 * @param res Response object to stringify.
 * @return Heap-allocated response string, or NULL on invalid input.
 */
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
        cwist_sstring_append_len(s, res->body->data, res->body->size);
    }
    return s;
}

/**
 * @brief Parse a raw HTTP request buffer into a CWIST request object.
 * @param raw_request NUL-terminated request buffer containing headers and optional body.
 * @return Parsed request object, or NULL on malformed input.
 */
/**
 * @brief Validate the combined Transfer-Encoding header list (RFC 9112 §6.1).
 * Headers are prepended during parsing, so the first TE node found is the
 * last one on the wire and carries the final coding.
 * @return 0 valid, 1 when the final coding is not chunked, 2 when an
 *         unsupported transfer coding is present.
 */
static int http_validate_transfer_encoding(cwist_http_header_node *headers) {
    bool seen = false;
    bool final_is_chunked = false;
    bool bad_coding = false;
    for (cwist_http_header_node *n = headers; n; n = n->next) {
        if (!n->key || !n->key->data || strcasecmp(n->key->data, "Transfer-Encoding") != 0) continue;
        const char *p = (n->value && n->value->data) ? n->value->data : "";
        bool any_token = false;
        bool last_chunked = false;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            const char *tok = p;
            while (*p && *p != ',') p++;
            const char *end = p;
            while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) end--;
            if (*p) p++;
            if (end == tok) continue;
            any_token = true;
            /* A token with parameters (e.g. "chunked;x=1") is not chunked. */
            last_chunked = ((size_t)(end - tok) == 7 && strncasecmp(tok, "chunked", 7) == 0);
            if (!last_chunked) bad_coding = true;
        }
        if (!seen) {
            final_is_chunked = any_token && last_chunked;
            seen = true;
        }
    }
    if (!seen) return 0;
    if (!final_is_chunked) return 1;
    if (bad_coding) return 2;
    return 0;
}

/**
 * @brief Internal helper to parse request when header_end is already known.
 * On NULL return, *err_out (when given) says whether the client deserves a
 * 4xx/5xx response or a quiet close (CWIST_HTTP_PARSE_EOF).
 */
static cwist_http_request *cwist_http_parse_request_with_header_end(const char *raw_request, const char *header_end, cwist_http_parse_error_t *err_out) {
    if (!raw_request || !header_end) return NULL;

    const char *line_start = raw_request;
    const char *line_end = cwist_simd_find_crlf(line_start, (size_t)(header_end - line_start + 2));
    if (!line_end || line_end > header_end) {
        if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
        return NULL;
    }

    cwist_http_request *req = cwist_http_request_create();
    if (!req) {
        if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
        return NULL;
    }

    // Fast path for root GET / HTTP/1.1\r\n
    if (line_start[0] == 'G' && line_start[1] == 'E' && line_start[2] == 'T' &&
        line_start[3] == ' ' && line_start[4] == '/' && line_start[5] == ' ' &&
        line_start[6] == 'H' && line_start[7] == 'T' && line_start[8] == 'T' &&
        line_start[9] == 'P' && line_start[10] == '/' && line_start[11] == '1' &&
        line_start[12] == '.' && line_start[13] == '1' && line_end == line_start + 14) {
        req->method = CWIST_HTTP_GET;
        cwist_sstring_borrow(req->path, "/", 1);
        cwist_sstring_borrow(req->query, "", 0);
        cwist_sstring_borrow(req->version, "HTTP/1.1", 8);
        req->keep_alive = true;
    } else {
        // 1. Request Line (Optimized: SIMD space search)
        const char *sp1 = cwist_simd_find_char(line_start, (size_t)(line_end - line_start), ' ');
        if (!sp1 || sp1 > line_end) { cwist_http_request_destroy(req); if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED; return NULL; }
        const char *sp2 = cwist_simd_find_char(sp1 + 1, (size_t)(line_end - (sp1 + 1)), ' ');
        if (!sp2 || sp2 > line_end) { cwist_http_request_destroy(req); if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED; return NULL; }

        req->method = cwist_http_string_to_method_len(line_start, sp1 - line_start);
        
        const char *path_start = sp1 + 1;
        const char *path_end = sp2;
        const char *query_sep = (const char *)memchr(path_start, '?', (size_t)(path_end - path_start));
        
        if (query_sep) {
            cwist_http_sstring_assign_arena(req->path, (cwist_arena_t *)req->arena, path_start, (size_t)(query_sep - path_start));
            cwist_http_sstring_assign_arena(req->query, (cwist_arena_t *)req->arena, query_sep + 1, (size_t)(path_end - (query_sep + 1)));
            req->query_params = cwist_query_map_create_in_arena(req->arena);
            if (req->query_params) {
                cwist_query_map_parse(req->query_params, req->query->data);
            }
        } else {
            cwist_http_sstring_assign_arena(req->path, (cwist_arena_t *)req->arena, path_start, (size_t)(path_end - path_start));
            cwist_http_sstring_assign_arena(req->query, (cwist_arena_t *)req->arena, "", 0);
        }

        cwist_http_sstring_assign_arena(req->version, (cwist_arena_t *)req->arena, sp2 + 1, (size_t)(line_end - (sp2 + 1)));
        if (strncmp(sp2 + 1, "HTTP/1.1", 8) == 0) {
            req->keep_alive = true;
        } else {
            req->keep_alive = false;
        }
    }

    // 2. Headers (SIMD colon and CRLF scanning + SWAR)
    /* RFC 9110/9112 validation state tracked during the single header pass. */
    bool has_host = false, host_dup = false, host_empty = false;
    bool cl_seen = false, cl_bad = false;
    size_t cl_value = 0;
    bool te_seen = false;
    const bool is_http11 = req->version->data && strncmp(req->version->data, "HTTP/1.1", 8) == 0;

    line_start = line_end + 2;
    while (line_start < header_end) {
        line_end = strstr(line_start, "\r\n");
        if (!line_end) break;

        const char *colon = cwist_simd_find_char(line_start, (size_t)(line_end - line_start), ':');
        if (colon) {
            size_t key_len = colon - line_start;
            const char *val_start = colon + 1;
            while (val_start < line_end && *val_start == ' ') val_start++;
            size_t val_len = line_end - val_start;

            cwist_http_header_add_ex_len(&req->headers, (cwist_arena_t *)req->arena, line_start, key_len, val_start, val_len);

            char k0 = line_start[0];
            if (key_len == 10 && (k0 == 'C' || k0 == 'c')) {
                uint64_t w0;
                memcpy(&w0, line_start, 8);
                if ((w0 | 0x2020202020202020ULL) == 0x697463656e6e6f63ULL) { /* "connecti" */
                    uint16_t w1;
                    memcpy(&w1, line_start + 8, 2);
                    if ((w1 | 0x2020) == 0x6e6f) { /* "on" */
                        if (val_len == 5 && (val_start[0] == 'c' || val_start[0] == 'C')) {
                            if (strncasecmp(val_start, "close", 5) == 0) req->keep_alive = false;
                        } else if (val_len == 10 && (val_start[0] == 'k' || val_start[0] == 'K')) {
                            if (strncasecmp(val_start, "keep-alive", 10) == 0) req->keep_alive = true;
                        }
                    }
                }
            } else if (key_len == 14 && (k0 == 'C' || k0 == 'c')) {
                if (strncasecmp(line_start, "Content-Length", 14) == 0) {
                    /* RFC 9112 §6.3: strict digits-only parse; duplicate CL is
                     * idempotent only when every value matches. */
                    size_t vlen = val_len;
                    while (vlen > 0 && (val_start[vlen - 1] == ' ' || val_start[vlen - 1] == '\t')) vlen--;
                    if (vlen == 0) {
                        cl_bad = true;
                    } else {
                        size_t len = 0;
                        bool valid = true;
                        for (size_t i = 0; i < vlen; i++) {
                            if (val_start[i] >= '0' && val_start[i] <= '9') {
                                if (len > (SIZE_MAX - 9) / 10) { valid = false; break; }
                                len = len * 10 + (size_t)(val_start[i] - '0');
                            } else {
                                valid = false;
                                break;
                            }
                        }
                        if (!valid) {
                            cl_bad = true;
                        } else {
                            if (cl_seen && cl_value != len) cl_bad = true;
                            cl_seen = true;
                            cl_value = len;
                            req->content_length = len;
                        }
                    }
                }
            } else if (key_len == 4 && (k0 == 'H' || k0 == 'h')) {
                if (strncasecmp(line_start, "Host", 4) == 0) {
                    if (has_host) host_dup = true;
                    has_host = true;
                    size_t vlen = val_len;
                    while (vlen > 0 && (val_start[vlen - 1] == ' ' || val_start[vlen - 1] == '\t')) vlen--;
                    if (vlen == 0) host_empty = true;
                }
            } else if (key_len == 17 && (k0 == 'T' || k0 == 't')) {
                if (strncasecmp(line_start, "Transfer-Encoding", 17) == 0) te_seen = true;
            }
            free(header_line);
        }
        
        line_start = line_end + 2;
    }

    /* RFC 9112 §3.2: HTTP/1.1 requests must carry exactly one non-empty Host. */
    if (is_http11 && (!has_host || host_dup || host_empty)) {
        cwist_http_request_destroy(req);
        if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
        return NULL;
    }
    if (cl_bad) {
        cwist_http_request_destroy(req);
        if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
        return NULL;
    }
    if (te_seen) {
        /* RFC 9112 §6.3: TE and CL together are a request-smuggling vector. */
        if (cl_seen) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
            return NULL;
        }
        int tev = http_validate_transfer_encoding(req->headers);
        if (tev != 0) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = (tev == 1) ? CWIST_HTTP_PARSE_MALFORMED
                                               : CWIST_HTTP_PARSE_TE_UNSUPPORTED;
            return NULL;
        }
    }
    /* RFC 9110 §10.1.1: only the 100-continue expectation is supported. */
    const char *expect = cwist_http_header_get(req->headers, "Expect");
    if (expect) {
        size_t elen = strlen(expect);
        while (elen > 0 && (expect[elen - 1] == ' ' || expect[elen - 1] == '\t')) elen--;
        if (!(elen == 12 && strncasecmp(expect, "100-continue", 12) == 0)) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_EXPECT_FAILED;
            return NULL;
        }
    }

    const char *body_start = header_end + 4;
    if (*body_start != '\0') {
        /* Pipelined body bytes already sitting in the read buffer; the
         * receive path adopts/replaces this with the full body later. */
        cwist_http_sstring_assign_arena(req->body, (cwist_arena_t *)req->arena, body_start, strlen(body_start));
    }

    if (err_out) *err_out = CWIST_HTTP_PARSE_OK;
    return req;
}

cwist_http_request *cwist_http_parse_request(const char *raw_request) {
    if (!raw_request) return NULL;
    size_t raw_len = strlen(raw_request);
    const char *header_end = cwist_simd_find_crlfcrlf(raw_request, raw_len);
    if (!header_end) return NULL;
    return cwist_http_parse_request_with_header_end(raw_request, header_end, NULL);
}



/**
 * @brief Read and reassemble a chunked transfer-encoded body.
 * @return 0 on success, -1 on error.
 */
static int http_parse_chunk_size(const char *line, size_t len, size_t *out_size) {
    size_t value = 0, digits = 0;
    for (size_t i = 0; i < len && line[i] != ';'; ++i) {
        unsigned char c = (unsigned char)line[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        if (value > (CWIST_HTTP_MAX_BODY_SIZE - digit) / 16) return -1;
        value = value * 16 + digit;
        ++digits;
    }
    if (!digits) return -1;
    *out_size = value;
    return 0;
}

static int http_read_chunked_body(int client_fd, char *buf, size_t *avail, size_t buf_cap, cwist_sstring *out) {
    size_t offset = 0;

    while (1) {
        char *crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
        while (!crlf) {
            if (*avail >= buf_cap - 1) return -1;
            ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0) return -1;
                    continue;
                }
                if (errno == EINTR) continue;
                return -1;
            }
            if (bytes == 0) return -1;
            *avail += (size_t)bytes;
            buf[*avail] = '\0';
            crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
        }

        size_t line_len = (size_t)(crlf - (buf + offset)) + 2;
        size_t chunk_size = 0;
        /* Strict hexadecimal parsing prevents accepting contaminated framing
         * such as `4junk` or signed/overflowed chunk lengths. */
        if (http_parse_chunk_size(buf + offset, line_len - 2, &chunk_size) != 0) return -1;
        offset += line_len;

        if (chunk_size == 0) {
            /* Consume optional trailers until empty line */
            while (offset + 1 < *avail && !(buf[offset] == '\r' && buf[offset + 1] == '\n')) {
                char *trailer_crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
                if (!trailer_crlf) {
                    if (*avail >= buf_cap - 1) return -1;
                    ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
                    if (bytes < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                            int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                            if (ret <= 0) return -1;
                            continue;
                        }
                        if (errno == EINTR) continue;
                        return -1;
                    }
                    if (bytes == 0) return -1;
                    *avail += (size_t)bytes;
                    buf[*avail] = '\0';
                    trailer_crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
                }
                if (trailer_crlf) {
                    offset = (size_t)(trailer_crlf - buf) + 2;
                }
            }
            if (offset + 1 < *avail && buf[offset] == '\r' && buf[offset + 1] == '\n') {
                offset += 2;
            }
            break;
        }

        if (chunk_size > CWIST_HTTP_MAX_BODY_SIZE || out->size + chunk_size > CWIST_HTTP_MAX_BODY_SIZE) return -1;

        while (*avail - offset < chunk_size + 2) {
            if (*avail >= buf_cap - 1) return -1;
            ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0) return -1;
                    continue;
                }
                if (errno == EINTR) continue;
                return -1;
            }
            if (bytes == 0) return -1;
            *avail += (size_t)bytes;
            buf[*avail] = '\0';
        }

        if (buf[offset + chunk_size] != '\r' || buf[offset + chunk_size + 1] != '\n') return -1;
        if (cwist_sstring_append_len(out, buf + offset, chunk_size).error.err_i8 != 0) return -1;
        offset += chunk_size + 2;
    }

    size_t leftover = *avail - offset;
    if (leftover > 0) {
        memmove(buf, buf + offset, leftover);
    }
    *avail = leftover;
    return 0;
}

cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len, cwist_http_parse_error_t *err_out) {
    if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
    size_t total_received = *buf_len;
    char *header_end = NULL;

    // 1. Read until headers are complete
    while (!(header_end = strstr(read_buf, "\r\n\r\n"))) {
        if (total_received >= buf_size - 1) {
            /* Fat Cookie/Authorization combinations can legitimately push a
             * header block past the read buffer; without this trail the drop
             * is indistinguishable from a client vanish. */
            cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_HTTP_HEADER_OVERFLOW);
            CWIST_LOG_WARN("[http] dropping connection: headers exceed %zu-byte read buffer", buf_size);
            if (err_out) *err_out = CWIST_HTTP_PARSE_HEADER_OVERFLOW;
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

    cwist_http_request *req = cwist_http_parse_request_with_header_end(read_buf, header_end, err_out);
    if (!req) return NULL;

    size_t header_len = (header_end + 4) - read_buf;
    size_t body_received = total_received - header_len;

    /* RFC 9110 §10.1.1: the parser already validated that any Expect value is
     * exactly "100-continue"; answer it before waiting on the body. */
    const char *te = cwist_http_header_get(req->headers, "Transfer-Encoding");
    const char *expect = cwist_http_header_get(req->headers, "Expect");
    if (expect && ((size_t)req->content_length > body_received || (te && req->content_length == 0))) {
        http_send_100_continue(client_fd);
    }

    // 2. Read body based on Content-Length or Transfer-Encoding
    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_BODY_TOO_LARGE;
            return NULL;
        }

        // Allocate body
        char *body = malloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
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
        /* Adopt the filled buffer: one allocation, zero copies. The partial
         * body the parser staged in the arena is simply superseded. */
        cwist_sstring_adopt_len(req->body, body, (size_t)req->content_length);

        // Calculate leftovers
        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(read_buf, header_end + 4 + req->content_length, leftover_len);
            *buf_len = leftover_len;
        } else {
            *buf_len = 0;
        }
    } else {
        /* A surviving Transfer-Encoding header was validated by the parser as
         * chunked-only with chunked final, so presence alone selects framing. */
        if (te) {
            if (body_received > 0) {
                memmove(read_buf, header_end + 4, body_received);
            }
            *buf_len = body_received;
            read_buf[*buf_len] = '\0';

            cwist_sstring *chunked = cwist_sstring_create();
            if (!chunked) {
                cwist_http_request_destroy(req);
                return NULL;
            }
            if (http_read_chunked_body(client_fd, read_buf, buf_len, buf_size, chunked) != 0) {
                cwist_sstring_destroy(chunked);
                cwist_http_request_destroy(req);
                if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
                return NULL;
            }
            /* Move the assembled buffer into the request body instead of
             * copying it a second time. */
            char *chunked_data = chunked->data;
            size_t chunked_len = chunked->size;
            chunked->data = NULL;
            chunked->size = 0;
            cwist_sstring_destroy(chunked);
            cwist_sstring_adopt_len(req->body, chunked_data, chunked_len);
        } else {
            *buf_len = 0;
        }
    }
    read_buf[*buf_len] = '\0';

    if (err_out) *err_out = CWIST_HTTP_PARSE_OK;
    return req;
}

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

/**
 * @brief Drain the socket into the connection stash.
 * Stops at EAGAIN, and also after a short read: poll is level-triggered, so
 * if bytes remain after a short recv the one-shot re-arm fires again
 * immediately.  This skips the guaranteed-EAGAIN second recv that otherwise
 * costs one wasted syscall per request on non-pipelined keep-alive traffic.
 * @return 0 on success (EAGAIN or data), -1 on orderly close or fatal error.
 */
int cwist_http_async_conn_fill(cwist_http_async_conn_t *conn) {
    for (;;) {
        if (conn->len + 1 >= conn->cap && !http_async_stash_grow(conn, conn->len + 4096)) {
            return -1;
        }
        size_t avail = conn->cap - 1 - conn->len;
        ssize_t n = recv(conn->fd, conn->rbuf + conn->len, avail, 0);
        if (n > 0) {
            conn->len += (size_t)n;
            conn->rbuf[conn->len] = '\0';
            conn->virgin = false;
            if ((size_t)n < avail) return 0; /* short read: drained for now */
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    // Status Line
    char status_line[256];
    snprintf(status_line, sizeof(status_line), "%s %d %s\r\n",
             res->version->data ? res->version->data : "HTTP/1.1",
             res->status_code,
             res->status_text->data ? res->status_text->data : "OK");
    cwist_sstring_append(response_str, status_line);

    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
            smartstring_append(response_str, curr->key->data);
            smartstring_append(response_str, ": ");
            smartstring_append(response_str, curr->value->data);
            smartstring_append(response_str, "\r\n");
        }
        curr = curr->next;
    }

    if (!headers_have_content_length(res->headers)) {
        char cl_header[64];
        snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", body_len);
        cwist_sstring_append(response_str, cl_header);
    }

    if (!headers_have_connection(res->headers)) {
        if (res->keep_alive) {
            cwist_sstring_append(response_str, "Connection: keep-alive\r\n");
        } else {
            cwist_sstring_append(response_str, "Connection: close\r\n");
        }
    }

    // End of headers
    cwist_sstring_append(response_str, "\r\n");

    // Body
    if (res->body && res->body->data) {
        cwist_sstring_append_len(response_str, res->body->data, res->body->size);
    }
    
    return response_str;
}

cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    cwist_sstring *response_str = cwist_http_stringify_response(res);
    if (!response_str) {
        err.error.err_i16 = -1;
        return err;
    }

    // Send
    err.error.err_i16 = 0;

    const char *p = response_str->data;
    size_t left = response_str->size;

    signal(SIGPIPE, SIG_IGN);

    while (left > 0) {
        struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
        int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
        if (ret <= 0) {
            err.error.err_i16 = -1;
            break;
        }

        ssize_t sent;
        #ifdef MSG_NOSIGNAL
        sent = send(client_fd, p, left, MSG_NOSIGNAL);
        #else
        sent = send(client_fd, p, left, 0);
        #endif

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            err.error.err_i16 = -1;
            break;
        }
        if (sent == 0) {
            err.error.err_i16 = -1;
            break;
        }

        p += (size_t)sent;
        left -= (size_t)sent;
    }

    cwist_sstring_destroy(response_str);
    return err;
}

/**
 * @brief Try to assemble one complete request from the stash without any
 * blocking IO.  On CWIST_RECV_OK the consumed bytes are removed from the
 * stash and *out holds a fully parsed request.
 */
cwist_recv_status_t cwist_http_receive_request_nb(cwist_http_async_conn_t *conn, cwist_http_request **out, cwist_http_parse_error_t *err_out) {
    *out = NULL;
    if (err_out) *err_out = CWIST_HTTP_PARSE_OK;
    if (!conn->rbuf || conn->len == 0) return CWIST_RECV_NEED_MORE;

    char *header_end = (char *)cwist_simd_find_crlfcrlf(conn->rbuf, conn->len);
    if (!header_end) {
        if (conn->len >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_HTTP_HEADER_OVERFLOW);
            CWIST_LOG_WARN("[http] dropping async connection: headers exceed %d-byte read buffer",
                           CWIST_HTTP_READ_BUFFER_SIZE);
            if (err_out) *err_out = CWIST_HTTP_PARSE_HEADER_OVERFLOW;
            return CWIST_RECV_FATAL;
        }
        return CWIST_RECV_NEED_MORE;
    }

    cwist_http_request *req = cwist_http_parse_request_with_header_end(conn->rbuf, header_end, err_out);
    if (!req) return CWIST_RECV_FATAL;

    size_t header_len = (size_t)(header_end + 4 - conn->rbuf);
    size_t body_received = conn->len - header_len;
    size_t consumed = header_len;

    /* RFC 9110 §10.1.1: answer a validated Expect: 100-continue once, before
     * the stash accumulates the full body. */
    const char *te = cwist_http_header_get(req->headers, "Transfer-Encoding");
    const char *expect = cwist_http_header_get(req->headers, "Expect");
    if (expect && !conn->expect_continue_sent &&
        ((size_t)req->content_length > body_received || (te && req->content_length == 0))) {
        http_send_100_continue(conn->fd);
        conn->expect_continue_sent = true;
    }

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_BODY_TOO_LARGE;
            return CWIST_RECV_FATAL;
        }
        if (body_received < (size_t)req->content_length) {
            /* Not all here yet; make sure the stash can hold the full message. */
            if (!http_async_stash_grow(conn, header_len + (size_t)req->content_length + 1)) {
                cwist_http_request_destroy(req);
                if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
                return CWIST_RECV_FATAL;
            }
            cwist_http_request_destroy(req);
            return CWIST_RECV_NEED_MORE;
        }
        char *body = cwist_alloc((size_t)req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
            return CWIST_RECV_FATAL;
        }
        memcpy(body, conn->rbuf + header_len, (size_t)req->content_length);
        body[req->content_length] = '\0';
        cwist_sstring_adopt_len(req->body, body, (size_t)req->content_length);
        consumed += (size_t)req->content_length;
    } else {
        /* Presence of TE implies parser-validated chunked framing. */
        if (te) {
            cwist_sstring *assembled = cwist_sstring_create();
            if (!assembled) {
                cwist_http_request_destroy(req);
                if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
                return CWIST_RECV_FATAL;
            }
            size_t chunk_bytes = 0;
            int scan = http_chunked_scan(conn->rbuf + header_len, body_received, &chunk_bytes, assembled);
            if (scan <= 0) {
                cwist_sstring_destroy(assembled);
                cwist_http_request_destroy(req);
                if (scan == 0) {
                    if (!http_async_stash_grow(conn, conn->len + 4096)) {
                        if (err_out) *err_out = CWIST_HTTP_PARSE_EOF;
                        return CWIST_RECV_FATAL;
                    }
                    return CWIST_RECV_NEED_MORE;
                }
                if (err_out) *err_out = CWIST_HTTP_PARSE_MALFORMED;
                return CWIST_RECV_FATAL;
            }
            char *data = assembled->data;
            size_t dlen = assembled->size;
            assembled->data = NULL;
            assembled->size = 0;
            cwist_sstring_destroy(assembled);
            cwist_sstring_adopt_len(req->body, data, dlen);
            consumed += chunk_bytes;
        }
    }

    /* Shift the leftover (pipelined bytes) to the stash head. */
    size_t leftover = conn->len - consumed;
    if (leftover > 0) memmove(conn->rbuf, conn->rbuf + consumed, leftover);
    conn->len = leftover;
    conn->rbuf[leftover] = '\0';
    conn->expect_continue_sent = false;

    req->client_fd = conn->fd;
    *out = req;
    return CWIST_RECV_OK;
}

/* --- End Non-blocking Request Assembly --- */

typedef struct {
    const char *ext;
    const char *mime;
} cwist_mime_entry;

static const cwist_mime_entry CWIST_MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "application/javascript" },
    { ".json", "application/json" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".ico",  "image/x-icon" }
};

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

cwist_error_t cwist_http_response_send_file(cwist_http_response *res, const char *file_path, const char *content_type_hint, size_t *out_size) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!res || !file_path) {
        err.error.err_i16 = -EINVAL;
        return err;
    }

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

    if ((size_t)st.st_size > CWIST_HTTP_MAX_BODY_SIZE) {
        close(fd);
        err.error.err_i16 = -EFBIG;
        return err;
    }

    size_t file_size = (size_t)st.st_size;
    char *buffer = NULL;

    if (!use_fast_stream && file_size > 0) {
        buffer = (char *)cwist_alloc(file_size + 1);
        if (!buffer) {
            close(fd);
            err.error.err_i16 = -ENOMEM;
            return err;
        }
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t bytes = read(fd, buffer + total_read, file_size - total_read);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            err.error.err_i16 = -errno;
            free(buffer);
            close(fd);
            return err;
        }
        if (bytes == 0) {
            err.error.err_i16 = -EIO;
            free(buffer);
            close(fd);
            return err;
        }
        total_read += (size_t)bytes;
    }
    close(fd);

        if (file_size > 0) {
            /* Adopt the read buffer as the body: no second file-size copy. */
            buffer[file_size] = '\0';
            cwist_sstring_adopt_len(res->body, buffer, file_size);
        } else {
            cwist_sstring_borrow(res->body, "", 0);
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

/* --- Socket Manipulation --- */

int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog) {
  int server_fd = -1;
  int opt = 1;
  in_addr_t addr = inet_addr(address);

  if(addr == INADDR_NONE) {
    return CWIST_HTTP_UNAVAILABLE_ADDRESS;
  }

  if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to create IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_CREATE_SOCKET_FAILED;
  }

  if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to set up IPv4 socket options");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_SETSOCKOPT_FAILED;  
  }

  sockv4->sin_family = AF_INET;
  sockv4->sin_addr.s_addr = addr;
  sockv4->sin_port = htons(port);

  if(bind(server_fd, (struct sockaddr *)sockv4, sizeof(struct sockaddr_in)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to bind IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_BIND_FAILED;
  }

  if(listen(server_fd, backlog) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    char err_msg[128];
    char err_format[128] = "Failed to listen at %s:%d";
    snprintf(err_msg, 127, err_format, address, port);

    cJSON_AddStringToObject(err_json, "err", err_msg);
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
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

cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd, void *), void *ctx) {
  int client_fd = -1;
  struct sockaddr_in peer_addr;
  socklen_t addrlen = sizeof(peer_addr);

  while(true) { 
    if((client_fd = accept(server_fd, (struct sockaddr *)&peer_addr, &addrlen)) < 0) {
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

    handler_func(client_fd, ctx);
  }

  cwist_error_t err = make_error(CWIST_ERR_INT16);
  err.error.err_i16 = -1;
  return err;
}

cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!config || server_fd < 0 || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    if (config->use_forking) {
        while (true) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -1;
                return err;
            }
            handle_client_forking(client_fd, handler, ctx);
        }
    }

    if (config->use_threading) {
        while (true) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                int accept_err = errno;
                if (accept_err == EINTR) continue;
                if (accept_err == EBADF || accept_err == EINVAL || accept_err == ENOTSOCK) break;
                if (cwist_accept_error_should_retry(accept_err)) {
                    cwist_accept_error_backoff(accept_err);
                    continue;
                }
                continue;
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
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
            close(epoll_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (true) {
            struct epoll_event events[16];
            int count = epoll_wait(epoll_fd, events, 16, -1);
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < count; i++) {
                if (events[i].data.fd == server_fd) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd >= 0) {
                        handler(client_fd, ctx);
                    }
                }
            }
        }
        close(epoll_fd);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (config->use_epoll) {
        int kqueue_fd = kqueue();
        if (kqueue_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct kevent change;
        EV_SET(&change, server_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(kqueue_fd, &change, 1, NULL, 0, NULL) < 0) {
            close(kqueue_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (true) {
            struct kevent events[16];
            int count = kevent(kqueue_fd, NULL, 0, events, 16, NULL);
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < count; i++) {
                if ((int)events[i].ident == server_fd) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd >= 0) {
                        handler(client_fd, ctx);
                    }
                }
            }
        }
        close(kqueue_fd);
    }
#endif

    return cwist_accept_socket(server_fd, NULL, handler, ctx);
}
