#define _POSIX_C_SOURCE 200809L
#include <cwist/http.h>
#include <cwist/sstring.h>
#include <cwist/err/cwist_err.h>

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

static void *http_pool_worker(void *arg) {
    http_thread_worker_t *w = (http_thread_worker_t *)arg;
    ttak_net_lattice_set_worker_id(w->worker_id);

    http_pool_task_t batch[16];

    while (1) {
        size_t batch_count = 0;
        pthread_mutex_lock(&w->mutex);
        while (w->count == 0 && !w->shutdown) {
            pthread_cond_wait(&w->cond_not_empty, &w->mutex);
        }
        if (w->shutdown && w->count == 0) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }

        while (w->count > 0 && batch_count < 16) {
            batch[batch_count++] = w->queue[w->head];
            w->head = (w->head + 1) % HTTP_TASKS_PER_THREAD;
            w->count--;
        }
        if (batch_count > 0) {
            pthread_cond_signal(&w->cond_not_full);
        }
        pthread_mutex_unlock(&w->mutex);

        for (size_t i = 0; i < batch_count; i++) {
            batch[i].handler_func(batch[i].client_fd, batch[i].ctx);
        }
    }
    return NULL;
}

int cwist_http_pool_init(void) {
    g_http_thread_count = get_optimal_thread_count();
    g_workers = cwist_alloc(g_http_thread_count * sizeof(http_thread_worker_t));
    g_rr_index = 0;
    memset(g_workers, 0, g_http_thread_count * sizeof(http_thread_worker_t));
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        g_workers[i].queue = cwist_alloc(HTTP_TASKS_PER_THREAD * sizeof(http_pool_task_t));
        if (!g_workers[i].queue) return -1;
        
        pthread_mutex_init(&g_workers[i].mutex, NULL);
        pthread_cond_init(&g_workers[i].cond_not_empty, NULL);
        pthread_cond_init(&g_workers[i].cond_not_full, NULL);
        g_workers[i].worker_id = (uint32_t)i;
        if (pthread_create(&g_workers[i].thread, NULL, http_pool_worker, &g_workers[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx) {
    /* Deterministic worker selection using Choi Seok-jeong's MOLS to minimize cache bouncing. */
    uint16_t node_id = (uint16_t)(client_fd % TTAK_MOLS_NODE_COUNT);
    uint32_t mixed = ttak_apply_mols_control(node_id, (uint32_t)g_rr_index);
    size_t worker_idx = mixed % get_optimal_thread_count();

    g_rr_index = (g_rr_index + 1) % get_optimal_thread_count();
    
    http_thread_worker_t *w = &g_workers[worker_idx];

    pthread_mutex_lock(&w->mutex);
    while (w->count >= HTTP_TASKS_PER_THREAD && !w->shutdown) {
        pthread_cond_wait(&w->cond_not_full, &w->mutex);
    }
    if (w->shutdown) {
        pthread_mutex_unlock(&w->mutex);
        close(client_fd);
        return;
    }
    w->queue[w->tail].client_fd = client_fd;
    w->queue[w->tail].handler_func = handler;
    w->queue[w->tail].ctx = ctx;
    w->tail = (w->tail + 1) % HTTP_TASKS_PER_THREAD;
    w->count++;
    pthread_cond_signal(&w->cond_not_empty);
    pthread_mutex_unlock(&w->mutex);
}
void cwist_http_pool_destroy(void) {
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        pthread_mutex_lock(&g_workers[i].mutex);
        g_workers[i].shutdown = 1;
        pthread_cond_broadcast(&g_workers[i].cond_not_empty);
        pthread_mutex_unlock(&g_workers[i].mutex);
    }
    for (int i = 0; i < get_optimal_thread_count(); i++) {
#if defined(__linux__)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += g_cwist_drain_timeout_sec;
        int rc = pthread_timedjoin_np(g_workers[i].thread, NULL, &ts);
        if (rc == ETIMEDOUT) {
            pthread_cancel(g_workers[i].thread);
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

    req->method = cwist_http_string_to_method_len(line_start, sp1 - line_start);
    
    const char *path_start = sp1 + 1;
    const char *path_end = sp2;
    const char *query_sep = memchr(path_start, '?', path_end - path_start);
    
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
    
    free(request_line);

    cwist_http_sstring_assign_arena(req->version, (cwist_arena_t *)req->arena, sp2 + 1, (size_t)(line_end - (sp2 + 1)));
    if (strncmp(sp2 + 1, "HTTP/1.1", 8) == 0) {
        req->keep_alive = true;
    } else {
        req->keep_alive = false;
    }

    // 2. Headers (Optimized: Zero-copy in-place parsing using memchr and strncasecmp)
    line_start = line_end + 2; 
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
        /* Pipelined body bytes already sitting in the read buffer; the
         * receive path adopts/replaces this with the full body later. */
        cwist_http_sstring_assign_arena(req->body, (cwist_arena_t *)req->arena, body_start, strlen(body_start));
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
        // No body, leftovers are everything after headers
        if (body_received > 0) {
            memmove(read_buf, header_end + 4, body_received);
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


cwist_sstring *cwist_http_stringify_response(cwist_http_response *res) {
    if (!res) return NULL;

    cwist_sstring *response_str = cwist_sstring_create();
    size_t body_len = 0;
    if (res->body) {
        body_len = res->body->size;
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
