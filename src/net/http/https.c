#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/https.h>
#include <cwist/net/http/writer_fast.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include "tls_chain.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

/* Forward declaration: PQC layer applied inside TLS bootstrap */
bool cwist_tls_apply_pqc_layer(cwist_app *app, SSL_CTX *ctx);

/* Bodies up to this size are written in the same TLS record as the
 * response headers; larger bodies amortize the per-record overhead. */
#define CWIST_TLS_COALESCE_MAX (16 * 1024)

/* Monotonic clock in milliseconds, for connection deadlines. */
static uint64_t cwist_https_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Poll the socket for the direction OpenSSL is waiting on.
 * @return 0 if the requested event is ready, -1 on timeout/error.
 */
static int cwist_ssl_wait(int fd, int ssl_error, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = 0 };
    if (ssl_error == SSL_ERROR_WANT_READ) {
        pfd.events = POLLIN;
    } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
        pfd.events = POLLOUT;
    } else {
        return -1;
    }

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 0;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    SSL *pres_ssl;  /* non-NULL when the shepherd already finished the handshake */
};

/* --- Thread Pool for HTTPS --- */
#define HTTPS_TASK_QUEUE_SIZE 2097152

typedef struct {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    SSL *pres_ssl;
} https_pool_task_t;

typedef struct {
    pthread_t threads[2048];
    size_t    threads_size;
    https_pool_task_t queue[HTTPS_TASK_QUEUE_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int shutdown;
} https_thread_pool_t;

static https_thread_pool_t g_https_pool;
static bool g_https_pool_initialized = false;

// Forward declaration of existing https_thread_handler
static void *https_thread_handler(void *arg);
void https_pool_submit_ready(int client_fd, SSL *pres_ssl, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx);
static cwist_error_t https_wrap_established(cwist_https_context *ctx, int client_fd, SSL *ssl, cwist_https_connection **conn);
int https_hs_shepherd_start(void);
void https_hs_shepherd_stop(void);

static void *https_pool_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_https_pool.mutex);
        while (g_https_pool.count == 0 && !g_https_pool.shutdown) {
            pthread_cond_wait(&g_https_pool.cond_not_empty, &g_https_pool.mutex);
        }
        if (g_https_pool.shutdown) {
            pthread_mutex_unlock(&g_https_pool.mutex);
            break;
        }
        https_pool_task_t task = g_https_pool.queue[g_https_pool.head];
        g_https_pool.head = (g_https_pool.head + 1) % HTTPS_TASK_QUEUE_SIZE;
        g_https_pool.count--;
        pthread_cond_signal(&g_https_pool.cond_not_full);
        pthread_mutex_unlock(&g_https_pool.mutex);

        // We can reuse the existing https_thread_handler logic by wrapping the task
        struct https_thread_payload *payload = malloc(sizeof(*payload));
        if (payload) {
            payload->client_fd = task.client_fd;
            payload->ctx = task.ctx;
            payload->handler = task.handler;
            payload->user_ctx = task.user_ctx;
            payload->pres_ssl = task.pres_ssl;
            https_thread_handler(payload);
        } else {
            if (task.pres_ssl) SSL_free(task.pres_ssl);
            close(task.client_fd);
        }
    }
    return NULL;
}

int https_pool_init(void) {
    if (g_https_pool_initialized) return 0;
    memset(&g_https_pool, 0, sizeof(g_https_pool));
    pthread_mutex_init(&g_https_pool.mutex, NULL);
    pthread_cond_init(&g_https_pool.cond_not_empty, NULL);
    pthread_cond_init(&g_https_pool.cond_not_full, NULL);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        if (pthread_create(&g_https_pool.threads[i], NULL, https_pool_worker, NULL) != 0) {
            return -1;
        }
    }
    g_https_pool_initialized = true;
    return 0;
}

void https_pool_submit(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    https_pool_submit_ready(client_fd, NULL, ctx, handler, user_ctx);
}

/* Submit a connection whose TLS handshake already completed (pres_ssl). */
void https_pool_submit_ready(int client_fd, SSL *pres_ssl, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    pthread_mutex_lock(&g_https_pool.mutex);
    while (g_https_pool.count >= HTTPS_TASK_QUEUE_SIZE && !g_https_pool.shutdown) {
        pthread_cond_wait(&g_https_pool.cond_not_full, &g_https_pool.mutex);
    }
    if (g_https_pool.shutdown) {
        pthread_mutex_unlock(&g_https_pool.mutex);
        if (pres_ssl) SSL_free(pres_ssl);
        close(client_fd);
        return;
    }
    g_https_pool.queue[g_https_pool.tail].client_fd = client_fd;
    g_https_pool.queue[g_https_pool.tail].ctx = ctx;
    g_https_pool.queue[g_https_pool.tail].handler = handler;
    g_https_pool.queue[g_https_pool.tail].user_ctx = user_ctx;
    g_https_pool.queue[g_https_pool.tail].pres_ssl = pres_ssl;
    g_https_pool.tail = (g_https_pool.tail + 1) % HTTPS_TASK_QUEUE_SIZE;
    g_https_pool.count++;
    pthread_cond_signal(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
}

void https_pool_destroy(void) {
    if (!g_https_pool_initialized) return;
    https_hs_shepherd_stop();
    pthread_mutex_lock(&g_https_pool.mutex);
    g_https_pool.shutdown = 1;
    pthread_cond_broadcast(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        pthread_join(g_https_pool.threads[i], NULL);
    }
    pthread_mutex_destroy(&g_https_pool.mutex);
    pthread_cond_destroy(&g_https_pool.cond_not_empty);
    pthread_cond_destroy(&g_https_pool.cond_not_full);

    g_https_pool_initialized = false;
}
/* --- End Thread Pool --- */

/* --- TLS handshake shepherd ------------------------------------------------
 * Why this exists: the pool parks one worker per connection for the whole
 * connection lifetime.  A synchronous in-worker handshake made that fatal
 * under churn — a client that delays its ClientHello by a few seconds
 * (routine for h2load at thousands of connections per process) parked a
 * worker for the full 30 s poll, the pool collapsed to ~tens of
 * handshakes/s, the accept loop fell behind, the kernel accept queue
 * overflowed, and overflowing handshakes were dropped silently, leaving
 * clients ESTABLISHED with no server-side socket ("phantom" connections).
 *
 * The shepherd owns every in-progress handshake: cwist_https_dispatch()
 * tries SSL_accept once on a non-blocking socket; incomplete handshakes are
 * parked in a private epoll set and retried by a single shepherd thread,
 * which also reaps connections that exceed CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS.
 * Only fully established sessions enter the worker pool.
 *
 * The shepherd is epoll-based and therefore Linux-only.  On other platforms
 * cwist_https_dispatch() falls back to the legacy blocking pool path. */

#ifdef __linux__

typedef struct https_hs_pending {
    int fd;
    SSL *ssl;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    uint64_t deadline_ms;
    struct https_hs_pending *next;
} https_hs_pending_t;

typedef struct https_hs_shard {
    int epoll_fd;
    int wakeup_rd;
    int wakeup_wr;
    pthread_t thread;
    bool running;
    pthread_mutex_t lock;
    https_hs_pending_t *head;
    _Atomic uint32_t pending;
} https_hs_shard_t;

/* One shepherd thread per shard; the count is fixed at start (see below). */
#define CWIST_HTTPS_HS_MAX_SHARDS 16

static https_hs_shard_t g_hs_shards[CWIST_HTTPS_HS_MAX_SHARDS];
static long g_hs_shard_count = 0;
static pthread_mutex_t g_hs_start_lock = PTHREAD_MUTEX_INITIALIZER;

/* Both called with sh->lock held; O(1), so the shepherd's per-event and
 * sweep costs no longer grow with the number of pending handshakes. */
static void https_hs_link_locked(https_hs_shard_t *sh, https_hs_pending_t *p) {
    p->next = NULL;
    p->prev = sh->tail;
    if (sh->tail)
        sh->tail->next = p;
    else
        sh->head = p;
    sh->tail = p;
    p->linked = true;
    atomic_fetch_add_explicit(&sh->pending, 1, memory_order_release);
}

static void https_hs_abort(https_hs_pending_t *p) {
    if (g_hs_epoll_fd >= 0) epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_DEL, p->fd, NULL);
    SSL_free(p->ssl);
    close(p->fd);
    cwist_free(p);
}

/* Move a completed handshake into the request worker pool. */
static void https_hs_complete(https_hs_pending_t *p) {
    if (g_hs_epoll_fd >= 0) epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_DEL, p->fd, NULL);
    /* The request phase (cwist_https_receive_request & friends) predates
     * non-blocking sockets; restore blocking mode before handing over. */
    int fl = fcntl(p->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(p->fd, F_SETFL, fl & ~O_NONBLOCK);
    https_pool_submit_ready(p->fd, p->ssl, p->ctx, p->handler, p->user_ctx);
    cwist_free(p);
}

static void *https_hs_shepherd(void *arg) {
    (void)arg;
    struct epoll_event events[512];
    while (g_hs_running) {
        int n = epoll_wait(g_hs_epoll_fd, events, 512, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        uint64_t now = cwist_https_now_ms();
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* wakeup pipe: drain so writers never block */
                char drain[256];
                while (read(g_hs_wakeup_rd, drain, sizeof(drain)) > 0) { /* discard */ }
                continue;
            }
            https_hs_pending_t *p = events[i].data.ptr;

            /* Detach from the list first: every event path below either
             * re-arms (re-add) or finishes with p freed. */
            pthread_mutex_lock(&g_hs_lock);
            https_hs_pending_t *prev = NULL, *cur = g_hs_head;
            bool found = false;
            while (cur) {
                if (cur == p) { https_hs_forget_locked(prev, cur); found = true; break; }
                prev = cur; cur = cur->next;
            }
            pthread_mutex_unlock(&g_hs_lock);
            if (!found) continue; /* already reaped by the sweeper */

            int rc = SSL_accept(p->ssl);
            if (rc > 0) {
                https_hs_complete(p);
                continue;
            }
            int ssl_err = SSL_get_error(p->ssl, rc);
            if ((ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) &&
                now < p->deadline_ms) {
                struct epoll_event ev = {
                    .events = (ssl_err == SSL_ERROR_WANT_WRITE ? EPOLLOUT : EPOLLIN) | EPOLLRDHUP,
                    .data.ptr = p,
                };
                pthread_mutex_lock(&g_hs_lock);
                p->next = g_hs_head;
                g_hs_head = p;
                atomic_fetch_add_explicit(&g_hs_pending_count, 1, memory_order_release);
                pthread_mutex_unlock(&g_hs_lock);
                if (epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_MOD, p->fd, &ev) != 0 &&
                    epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_ADD, p->fd, &ev) != 0) {
                    pthread_mutex_lock(&g_hs_lock);
                    prev = NULL; cur = g_hs_head;
                    while (cur) {
                        if (cur == p) { https_hs_forget_locked(prev, cur); break; }
                        prev = cur; cur = cur->next;
                    }
                    pthread_mutex_unlock(&g_hs_lock);
                    https_hs_abort(p);
                }
                continue;
            }
            https_hs_abort(p); /* hard failure or deadline exceeded */
        }

        /* Sweep expired handshakes that never became readable. */
        uint64_t sweep_now = cwist_https_now_ms();
        pthread_mutex_lock(&g_hs_lock);
        https_hs_pending_t *prev = NULL, *cur = g_hs_head;
        while (cur) {
            https_hs_pending_t *next = cur->next;
            if (sweep_now >= cur->deadline_ms) {
                https_hs_forget_locked(prev, cur);
                pthread_mutex_unlock(&g_hs_lock);
                https_hs_abort(cur);
                pthread_mutex_lock(&g_hs_lock);
                cur = prev ? prev->next : g_hs_head;
                continue;
            }
            prev = cur;
            cur = next;
        }
        pthread_mutex_unlock(&g_hs_lock);
    }
    return NULL;
}

int https_hs_shepherd_start(void) {
    pthread_mutex_lock(&g_hs_lock);
    if (g_hs_running) {
        pthread_mutex_unlock(&g_hs_lock);
        return 0;
    }
    g_hs_epoll_fd = epoll_create1(0);
    if (g_hs_epoll_fd < 0) {
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) {
        close(g_hs_epoll_fd);
        g_hs_epoll_fd = -1;
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    g_hs_wakeup_rd = pfd[0];
    g_hs_wakeup_wr = pfd[1];
    fcntl(g_hs_wakeup_rd, F_SETFL, O_NONBLOCK);
    fcntl(g_hs_wakeup_wr, F_SETFL, O_NONBLOCK);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_ADD, g_hs_wakeup_rd, &ev);
    g_hs_running = true;
    if (pthread_create(&g_hs_thread, NULL, https_hs_shepherd, NULL) != 0) {
        g_hs_running = false;
        close(g_hs_wakeup_rd);
        close(g_hs_wakeup_wr);
        close(g_hs_epoll_fd);
        g_hs_epoll_fd = -1;
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_hs_lock);
    return 0;
}

void https_hs_shepherd_stop(void) {
    pthread_mutex_lock(&g_hs_lock);
    if (!g_hs_running) {
        pthread_mutex_unlock(&g_hs_lock);
        return;
    }
    g_hs_running = false;
    if (g_hs_wakeup_wr >= 0) {
        char b = 1;
        if (write(g_hs_wakeup_wr, &b, 1) < 0) { /* shutdown path */ }
    }
    pthread_mutex_unlock(&g_hs_lock);
    pthread_join(g_hs_thread, NULL);

    pthread_mutex_lock(&g_hs_lock);
    https_hs_pending_t *cur = g_hs_head;
    g_hs_head = NULL;
    pthread_mutex_unlock(&g_hs_lock);
    while (cur) {
        https_hs_pending_t *next = cur->next;
        SSL_free(cur->ssl);
        close(cur->fd);
        cwist_free(cur);
        cur = next;
    }
    if (sh->epoll_fd >= 0) close(sh->epoll_fd);
    if (sh->wakeup_rd >= 0) close(sh->wakeup_rd);
    if (sh->wakeup_wr >= 0) close(sh->wakeup_wr);
    sh->epoll_fd = -1;
    sh->wakeup_rd = sh->wakeup_wr = -1;
}

int https_hs_shepherd_start(void) {
    pthread_mutex_lock(&g_hs_start_lock);
    if (g_hs_shard_count > 0) {
        pthread_mutex_unlock(&g_hs_start_lock);
        return 0;
    }
    /* Spread handshakes across as many shepherd threads as this process has
     * request workers (bounded), matching the parallelism of the legacy
     * blocking pool path without parking request workers on handshakes. */
    long want = get_optimal_thread_count();
    if (want < 4) want = 4;
    if (want > CWIST_HTTPS_HS_MAX_SHARDS) want = CWIST_HTTPS_HS_MAX_SHARDS;
    /* CWIST_HTTPS_HS_SHARDS overrides the computed count for connect-burst
     * tuning; values outside [1, CWIST_HTTPS_HS_MAX_SHARDS] are ignored. */
    const char *env = getenv("CWIST_HTTPS_HS_SHARDS");
    if (env && *env) {
        char *end = NULL;
        long parsed = strtol(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 1 && parsed <= CWIST_HTTPS_HS_MAX_SHARDS)
            want = parsed;
    }

    long started = 0;
    for (long i = 0; i < want; i++) {
        https_hs_shard_t *sh = &g_hs_shards[i];
        memset(sh, 0, sizeof(*sh));
        sh->epoll_fd = -1;
        sh->wakeup_rd = sh->wakeup_wr = -1;
        pthread_mutex_init(&sh->lock, NULL);
        atomic_init(&sh->pending, 0);

        sh->epoll_fd = epoll_create1(0);
        if (sh->epoll_fd < 0) break;
        int pfd[2];
        if (pipe(pfd) != 0) {
            close(sh->epoll_fd);
            sh->epoll_fd = -1;
            break;
        }
        sh->wakeup_rd = pfd[0];
        sh->wakeup_wr = pfd[1];
        fcntl(sh->wakeup_rd, F_SETFL, O_NONBLOCK);
        fcntl(sh->wakeup_wr, F_SETFL, O_NONBLOCK);
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
        epoll_ctl(sh->epoll_fd, EPOLL_CTL_ADD, sh->wakeup_rd, &ev);
        sh->running = true;
        if (pthread_create(&sh->thread, NULL, https_hs_shepherd, sh) != 0) {
            sh->running = false;
            close(sh->wakeup_rd);
            close(sh->wakeup_wr);
            close(sh->epoll_fd);
            sh->epoll_fd = -1;
            sh->wakeup_rd = sh->wakeup_wr = -1;
            break;
        }
        started++;
    }
    if (started == 0) {
        pthread_mutex_unlock(&g_hs_start_lock);
        return -1;
    }
    g_hs_shard_count = started;
    pthread_mutex_unlock(&g_hs_start_lock);
    return 0;
}

void https_hs_shepherd_stop(void) {
    pthread_mutex_lock(&g_hs_start_lock);
    long count = g_hs_shard_count;
    g_hs_shard_count = 0;
    pthread_mutex_unlock(&g_hs_start_lock);
    for (long i = 0; i < count; i++) {
        https_hs_shard_stop(&g_hs_shards[i]);
    }
}

long cwist_https_pending_handshakes(void) {
    return atomic_load_explicit(&g_hs_pending_count, memory_order_acquire);
}

/**
 * @brief Non-blocking TLS accept: offload the handshake immediately to
 * shepherd shards without blocking the accept thread with synchronous crypto.
 * Uses Power of Two Choices (P2C) to distribute pending handshakes evenly
 * across shards without lock contention or thread migration overhead.
 * Safe to call from any thread, including reactor callbacks.
 */
void cwist_https_dispatch(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    if (!ctx || !ctx->ctx || client_fd < 0) {
        if (client_fd >= 0) close(client_fd);
        return;
    }
    if (https_hs_shepherd_start() != 0) {
        /* No shepherd, no dispatch: fall back to the blocking pool path. */
        https_pool_submit(client_fd, ctx, handler, user_ctx);
        return;
    }

    int fl = fcntl(client_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        close(client_fd);
        return;
    }
    SSL_set_fd(ssl, client_fd);

    https_hs_pending_t *p = cwist_alloc(sizeof(*p));
    if (!p) {
        SSL_free(ssl);
        close(client_fd);
        return;
    }
    p->fd = client_fd;
    p->ssl = ssl;
    p->ctx = ctx;
    p->handler = handler;
    p->user_ctx = user_ctx;
    p->deadline_ms = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;

    /* P2C (Power of Two Choices) Shard Selection:
     * Samples two shepherd shards and assigns to the one with lower pending count. */
    unsigned shard_idx = 0;
    if (g_hs_shard_count > 1) {
        _Atomic uint32_t pending_arr[CWIST_HTTPS_HS_MAX_SHARDS];
        for (long i = 0; i < g_hs_shard_count; i++) {
            atomic_init(&pending_arr[i], atomic_load_explicit(&g_hs_shards[i].pending, memory_order_relaxed));
        }
        shard_idx = (unsigned)cwist_sched_p2c_select_worker(pending_arr, (uint32_t)g_hs_shard_count);
    }

    https_hs_shard_t *sh = &g_hs_shards[shard_idx];
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
        .data.ptr = p,
    };

    pthread_mutex_lock(&sh->lock);
    p->next = sh->head;
    sh->head = p;
    atomic_fetch_add_explicit(&sh->pending, 1, memory_order_release);
    pthread_mutex_unlock(&sh->lock);

    if (epoll_ctl(sh->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
        pthread_mutex_lock(&sh->lock);
        https_hs_pending_t *prev = NULL, *cur = sh->head;
        while (cur) {
            if (cur == p) { https_hs_forget_locked(prev, cur); break; }
            prev = cur; cur = cur->next;
        }
        pthread_mutex_unlock(&g_hs_lock);
        SSL_free(ssl);
        close(client_fd);
        cwist_free(p);
        return;
    }
    /* Wake the shepherd so pending handshakes are processed promptly. */
    char b = 1;
    if (write(g_hs_wakeup_wr, &b, 1) < 0) { /* non-fatal */ }
}

#else /* !__linux__ */

int https_hs_shepherd_start(void) { return -1; }
void https_hs_shepherd_stop(void) {}

long cwist_https_pending_handshakes(void) { return 0; }

/* Non-Linux fallback: no epoll shepherd, use the legacy blocking pool path. */
void cwist_https_dispatch(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    if (!ctx || !ctx->ctx || client_fd < 0) {
        if (client_fd >= 0) close(client_fd);
        return;
    }
    https_pool_submit(client_fd, ctx, handler, user_ctx);
}

#endif /* __linux__ */

#define CWIST_ALPN_HTTP11       ((const unsigned char *)"\x08http/1.1")
#define CWIST_ALPN_H2_HTTP11    ((const unsigned char *)"\x02h2\x08http/1.1")
#define CWIST_ALPN_H3_H2_HTTP11 ((const unsigned char *)"\x02h3\x02h2\x08http/1.1")
#define CWIST_ALPN_HTTP11_LEN       9
#define CWIST_ALPN_H2_HTTP11_LEN    12
#define CWIST_ALPN_H3_H2_HTTP11_LEN 15

/**
 * @file https.c
 * @brief OpenSSL-backed HTTPS accept, receive, send, and server-loop helpers.
 */

/**
 * @brief Build a JSON-rich cwist_error_t from the latest OpenSSL error state.
 * @param msg Human-readable message describing the failing HTTPS step.
 * @return Error object with module, message, and OpenSSL error string fields.
 */
static cwist_error_t make_ssl_error(const char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    unsigned long ssl_err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(ssl_err, buf, sizeof(buf));

    cJSON_AddStringToObject(err.error.err_json, "module", "https");
    cJSON_AddStringToObject(err.error.err_json, "message", msg);
    cJSON_AddStringToObject(err.error.err_json, "openssl_error", buf);

    return err;
}

/**
 * @brief Initialize OpenSSL and create a server TLS context from PEM files.
 * @param ctx Output pointer that receives the allocated HTTPS context.
 * @param cert_path Path to the PEM certificate chain.
 * @param key_path Path to the PEM private key.
 * @return Tagged CWIST error describing success or failure.
 */
static void cwist_https_apply_base_tls_defaults(SSL_CTX *ssl_ctx) {
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
}

static cwist_error_t cwist_https_apply_http2_tls_profile(SSL_CTX *ssl_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (SSL_CTX_set_cipher_list(ssl_ctx,
                                "ECDHE-ECDSA-AES128-GCM-SHA256:"
                                "ECDHE-RSA-AES128-GCM-SHA256:"
                                "ECDHE-ECDSA-AES256-GCM-SHA384:"
                                "ECDHE-RSA-AES256-GCM-SHA384:"
                                "ECDHE-ECDSA-CHACHA20-POLY1305:"
                                "ECDHE-RSA-CHACHA20-POLY1305") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.2 cipher profile");
    }
#ifdef SSL_CTX_set_ciphersuites
    if (SSL_CTX_set_ciphersuites(ssl_ctx,
                                 "TLS_AES_128_GCM_SHA256:"
                                 "TLS_AES_256_GCM_SHA384:"
                                 "TLS_CHACHA20_POLY1305_SHA256") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.3 cipher suites");
    }
#endif
    SSL_CTX_set_options(ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    err.error.err_i16 = 0;
    return err;
}

static int cwist_https_alpn_select_cb(SSL *ssl,
                                      const unsigned char **out,
                                      unsigned char *outlen,
                                      const unsigned char *in,
                                      unsigned int inlen,
                                      void *arg) {
    (void)ssl;
    const cwist_https_context *hctx = (const cwist_https_context *)arg;
    bool enable_http2 = hctx && hctx->http2_enabled;

    const unsigned char *supported;
    unsigned int supported_len;

    if (enable_http2) {
        supported = CWIST_ALPN_H2_HTTP11;
        supported_len = CWIST_ALPN_H2_HTTP11_LEN;
    } else {
        supported = CWIST_ALPN_HTTP11;
        supported_len = CWIST_ALPN_HTTP11_LEN;
    }

    if (SSL_select_next_proto((unsigned char **)out,
                              outlen,
                              supported,
                              supported_len,
                              in,
                              inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

/* --- Context Management --- */

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    return cwist_https_init_context_with_options(ctx, cert_path, key_path, NULL);
}

cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    bool enable_http3 = options && options->enable_http3;
    bool enable_http2 = options && (options->enable_http2 || enable_http3);
    
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    // Initialize OpenSSL
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return make_ssl_error("Unable to create SSL context");
    }

    if (SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to enforce TLS minimum version");
    }
    cwist_https_apply_base_tls_defaults(ssl_ctx);

    if (enable_http2) {
        err = cwist_https_apply_http2_tls_profile(ssl_ctx);
        if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) {
            SSL_CTX_free(ssl_ctx);
            return err;
        }
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to complete certificate chain");
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load private key");
    }

    // Verify key matches cert
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Private key does not match certificate");
    }

    *ctx = (cwist_https_context *)cwist_alloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;
    (*ctx)->http2_enabled = enable_http2;
    (*ctx)->http3_enabled = enable_http3;

    SSL_CTX_set_alpn_select_cb(ssl_ctx,
                               cwist_https_alpn_select_cb,
                               (void *)*ctx);

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Free an HTTPS context and release its OpenSSL resources.
 * @param ctx Context to destroy.
 */
void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        if (ctx->ticket_key) {
            /* Scrub key material before releasing it. */
            OPENSSL_cleanse(ctx->ticket_key, sizeof(cwist_tls_ticket_key));
            cwist_free(ctx->ticket_key);
        }
        cwist_free(ctx);
        /* EVP_cleanup() intentionally not called: it is deprecated since
         * OpenSSL 1.1.0 and tears down global state other code may use. */
    }
}

/**
 * @brief Wrap an accepted TCP client socket in an OpenSSL connection object.
 * @param ctx HTTPS context holding the configured SSL_CTX.
 * @param client_fd Accepted TCP socket descriptor.
 * @param conn Output pointer that receives the allocated connection wrapper.
 * @return Tagged CWIST error describing success or failure.
 */
/* Build the connection wrapper around an already-established TLS session.
 * Shared by the blocking cwist_https_accept path and the shepherd's
 * non-blocking dispatch path. */
static cwist_error_t https_wrap_established(cwist_https_context *ctx, int client_fd, SSL *ssl, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    *conn = (cwist_https_connection*)cwist_alloc(sizeof(cwist_https_connection));
    if (!*conn) {
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        cwist_free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';
    (*conn)->negotiated_http2 = false;
    (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP11;
    (*conn)->http3_enabled = ctx->http3_enabled;

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        (*conn)->negotiated_http2 = true;
        (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2;
    }
    /* h3 is QUIC-only and never negotiated over TCP TLS */

    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!ctx || !ctx->ctx || client_fd < 0) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        return make_ssl_error("Failed to create SSL structure");
    }

    SSL_set_fd(ssl, client_fd);
    cwist_tcp_quickack(client_fd);

    /* Bound the whole handshake so a client dribbling bytes cannot pin a
     * pool worker forever. */
    uint64_t handshake_deadline = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;
    int rc;
    while ((rc = SSL_accept(ssl)) <= 0) {
        int ssl_err = SSL_get_error(ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            uint64_t now = cwist_https_now_ms();
            if (now >= handshake_deadline) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out");
                SSL_free(ssl);
                return err_obj;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t remaining = handshake_deadline - now;
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;
            if (cwist_ssl_wait(client_fd, ssl_err, wait_ms) != 0) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out or socket error");
                SSL_free(ssl);
                return err_obj;
            }
            continue;
        }
        cwist_error_t err_obj = make_ssl_error("SSL handshake failed");
        SSL_free(ssl);
        return err_obj;
    }

    cwist_error_t wrap_err = https_wrap_established(ctx, client_fd, ssl, conn);
    if (wrap_err.errtype == CWIST_ERR_INT16 && wrap_err.error.err_i16 != 0) {
        SSL_free(ssl);
        return wrap_err;
    }
    return wrap_err;
}

/**
 * @brief Gracefully close an HTTPS connection and free its buffers.
 * @param conn HTTPS connection wrapper to close.
 */
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn) {
    return conn && conn->negotiated_protocol == CWIST_HTTPS_PROTOCOL_HTTP2;
}

cwist_https_protocol cwist_https_connection_protocol(const cwist_https_connection *conn) {
    if (!conn) return CWIST_HTTPS_PROTOCOL_NONE;
    return conn->negotiated_protocol;
}

void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        /* Balances the increment at wrap time; teardown is the single exit
         * point for every established connection (public close and the
         * full-GC registry sweep alike). */
        atomic_fetch_sub_explicit(&g_tls_connections_active, 1, memory_order_relaxed);
        if (conn->proto_state && conn->proto_state_free) {
            conn->proto_state_free(conn->proto_state);
            conn->proto_state = NULL;
        }
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            /* close() on a socket with unread receive-queue data makes the
             * kernel answer with RST instead of a graceful FIN. TLS clients
             * routinely send their close_notify (or a pipelined next request)
             * that the request path never consumed, so under connection churn
             * nearly every teardown became an abortive close: the peer's
             * kernel flushes queued response bytes on the RST (the "read
             * error" burst load clients report) and the socket never settles
             * through TIME_WAIT normally. Drain whatever is pending before
             * closing; the socket is about to be destroyed anyway. */
            char drain[2048];
            ssize_t n;
            int guard = 16;
            while (guard-- > 0 && (n = recv(conn->fd, drain, sizeof(drain), MSG_DONTWAIT)) > 0) {
                /* discard */
            }
            (void)n;
            close(conn->fd);
        }
        cwist_free(conn->read_buf);
        cwist_free(conn);
    }
}

/**
 * @brief Read from the TLS stream until a full HTTP request has been assembled.
 * @param conn Active HTTPS connection wrapper.
 * @return Parsed HTTP request, or NULL on timeout, parse failure, or IO failure.
 */
cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl || !conn->read_buf) return NULL;

    size_t total_received = conn->buf_len;
    char *header_end = NULL;

    /* Total deadline for assembling the request headers. */
    uint64_t headers_deadline = cwist_https_now_ms() + CWIST_HTTP_HEADERS_TIMEOUT_MS;

    while (!(header_end = strstr(conn->read_buf, "\r\n\r\n"))) {
        if (total_received >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            return NULL;
        }

        if (SSL_pending(conn->ssl) == 0) {
            uint64_t now = cwist_https_now_ms();
            if (now >= headers_deadline) {
                return NULL;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t remaining = headers_deadline - now;
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;

            struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
            int pret = poll(&pfd, 1, wait_ms);
            if (pret <= 0) {
                return NULL;
            }
        }

        int bytes = SSL_read(conn->ssl, conn->read_buf + total_received,
                             (int)(CWIST_HTTP_READ_BUFFER_SIZE - 1 - total_received));
        if (bytes <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, bytes);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) return NULL;
                continue;
            }
            return NULL;
        }

        total_received += (size_t)bytes;
        conn->read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(conn->read_buf);
    if (!req) return NULL;

    req->client_fd = conn->fd;
    req->https_conn = conn;

    size_t header_len = (header_end + 4) - conn->read_buf;
    size_t body_received = total_received - header_len;

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        char *body = cwist_alloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = body_received < req->content_length ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        /* No total cap (slow 1 GiB uploads must keep working); abort only
         * when no bytes arrive for a cumulative idle span.  Any successful
         * read resets the idle clock. */
        uint64_t body_idle_start = cwist_https_now_ms();

        while (current_body_len < req->content_length) {
            if (SSL_pending(conn->ssl) == 0) {
                uint64_t now = cwist_https_now_ms();
                if (now - body_idle_start >= CWIST_HTTP_BODY_IDLE_TIMEOUT_MS) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
                int wait_ms = CWIST_HTTP_TIMEOUT_MS;
                uint64_t idle_left = CWIST_HTTP_BODY_IDLE_TIMEOUT_MS - (now - body_idle_start);
                if (idle_left < (uint64_t)wait_ms) wait_ms = (int)idle_left;

                struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
                int pret = poll(&pfd, 1, wait_ms);
                if (pret <= 0) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len,
                                 (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) {
                        cwist_free(body);
                        cwist_http_request_destroy(req);
                        return NULL;
                    }
                    continue;
                }
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
            body_idle_start = cwist_https_now_ms();
        }
        body[req->content_length] = '\0';
        /* Adopt the filled buffer: one allocation, zero copies. */
        cwist_sstring_adopt_len(req->body, body, req->content_length);

        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(conn->read_buf, header_end + 4 + req->content_length, leftover_len);
            conn->buf_len = leftover_len;
        } else {
            conn->buf_len = 0;
        }
    } else {
        if (body_received > 0) {
            memmove(conn->read_buf, header_end + 4, body_received);
            conn->buf_len = body_received;
        } else {
            conn->buf_len = 0;
        }
    }
    conn->read_buf[conn->buf_len] = '\0';

    return req;
}

/**
 * @brief Serialize an HTTP response and send it over an active TLS connection.
 * @param conn Active HTTPS connection wrapper.
 * @param res Response object to serialize.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // Inject Alt-Svc when HTTP/3 is enabled so clients discover the QUIC endpoint
    if (conn->http3_enabled) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int port = 443;
        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
    }

    // 1+2. Headers onto a stack buffer; a small body rides in the same TLS
    // record, saving one record's AEAD tag and one syscall on the common
    // short-response path. Larger bodies keep the split writes (BoringSSL
    // records a big SSL_write at 16 KiB internally).
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));

    const char *body_ptr = NULL;
    size_t body_len = 0;
    if (res->is_ptr_body) {
        body_ptr = (const char *)res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body_ptr = res->body->data;
        body_len = res->body->size;
    }

    bool body_coalesced = false;
    if (body_ptr && body_len > 0 && body_len <= CWIST_TLS_COALESCE_MAX) {
        char *combined = (char *)cwist_alloc(header_len + body_len);
        if (combined) {
            memcpy(combined, header_buf, header_len);
            memcpy(combined + header_len, body_ptr, body_len);
            int rc = cwist_ssl_write_all(conn, combined, header_len + body_len);
            cwist_free(combined);
            if (rc != 0) {
                return make_ssl_error("SSL coalesced write failed");
            }
            body_coalesced = true;
        }
        /* Allocation failure: fall through to the split writes. */
    }
    if (!body_coalesced) {
        if (cwist_ssl_write_all(conn, header_buf, header_len) != 0) {
            return make_ssl_error("SSL header write failed");
        }
        if (body_ptr && body_len > 0) {
            if (cwist_ssl_write_all(conn, body_ptr, body_len) != 0) {
                return make_ssl_error("SSL body write failed");
            }
        }
    }

    // 3. File streams cannot use sendfile() through userland TLS; chunk them
    if (res->use_file_stream && res->file_stream_fd >= 0) {
        char fbuf[65536];
        size_t remaining = res->file_stream_len;
        off_t offset = res->file_stream_offset;
        while (remaining > 0) {
            size_t to_read = remaining < sizeof(fbuf) ? remaining : sizeof(fbuf);
            ssize_t n = pread(res->file_stream_fd, fbuf, to_read, offset);
            if (n < 0) {
                if (errno == EINTR) continue;
                return make_ssl_error("file stream read failed");
            }
            if (n == 0) break;
            if (cwist_ssl_write_all(conn, fbuf, (size_t)n) != 0) {
                return make_ssl_error("SSL file stream write failed");
            }
            offset += n;
            remaining -= (size_t)n;
        }
        res->file_stream_offset = offset;
    }

    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_https_send_response_head(cwist_https_connection *conn,
                                             cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 2. Send over SSL
    const char *p = response_str->data;
    int left = (int)response_str->size;
    int total_sent = 0;

    err.error.err_i16 = 0; // Assume success initially

    while (left > 0) {
        int sent = SSL_write(conn->ssl, p, left);
        if (sent <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, sent);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) {
                    err = make_ssl_error("SSL write timed out or socket error");
                    break;
                }
                continue; // Retry
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
    }

    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));
    if (cwist_ssl_write_all(conn, header_buf, header_len) != 0) {
        return make_ssl_error("SSL header write failed");
    }

    err.error.err_i16 = 0;
    return err;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
};

/**
 * @brief Worker entry point that performs the TLS handshake before dispatching.
 * @param arg Thread payload containing the accepted socket and dispatch callback.
 * @return Always NULL for pthread compatibility.
 */
static void *https_thread_handler(void *arg) {
    struct https_thread_payload *payload = (struct https_thread_payload *)arg;
    cwist_https_connection *conn = NULL;
    cwist_error_t hs_err;

    if (payload->pres_ssl) {
        /* Handshake already completed by the shepherd thread. */
        hs_err = https_wrap_established(payload->ctx, payload->client_fd, payload->pres_ssl, &conn);
        if (!(hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0)) {
            SSL_free(payload->pres_ssl);
        }
    } else {
        hs_err = cwist_https_accept(payload->ctx, payload->client_fd, &conn);
    }

    if (hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0) {
        payload->handler(conn, payload->user_ctx);
        if (t_https_parked) {
            t_https_parked = false;
        } else if (!conn->deferred) {
            cwist_https_close_connection(conn);
        }
    } else {
        if (hs_err.errtype == CWIST_ERR_JSON) {
            cJSON_Delete(hs_err.error.err_json);
        }
        if (payload->conn) {
            cwist_https_close_connection(payload->conn);
        } else {
            close(payload->client_fd);
        }
    }

    cwist_free(payload);
    return NULL;
}

/**
 * @brief Accept HTTPS clients in a loop and dispatch each one to the supplied handler.
 * @param server_fd Bound listening socket descriptor.
 * @param ctx HTTPS context shared by all accepted connections.
 * @param handler Callback invoked for each successful TLS client wrapper.
 * @param user_ctx Opaque pointer forwarded to the handler.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    while (atomic_load(&g_cwist_running)) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            continue;
        }

        /* Reap vanished peers within ~2 minutes instead of the ~2h kernel
         * default, so dead connections cannot park pool workers forever. */
        {
            int one = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
            int keepidle = 60;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
            int keepintvl = 10;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
            int keepcnt = 6;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
        }

        cwist_https_dispatch(client_fd, ctx, handler, user_ctx);
    }

    https_pool_destroy();
    return err;
}
