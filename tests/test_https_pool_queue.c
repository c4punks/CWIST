/**
 * @file test_https_pool_queue.c
 * @brief The HTTPS pool's task ring grows on demand and keeps FIFO order.
 *
 * One pool thread is held inside a handler while thousands of tasks are
 * queued behind it, forcing the ring through several doublings, including
 * growth while the ring has wrapped. Tasks carry a sequence number in
 * user_ctx; every one must run exactly once, in submission order. The
 * connections are inert stand-ins (no fd, no SSL), so teardown is cheap.
 */
#include <cwist/net/http/https.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TASKS 5000

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static bool g_gate_open = false;
static atomic_bool g_blocker_running = false;
static long g_seen[TASKS];
static atomic_long g_seen_n = 0;
/* Bumped after g_seen[] is written, so a reader that waits on it sees the
 * slot (g_seen_n only reserves the slot). */
static atomic_long g_done_n = 0;

static cwist_https_connection *fake_conn(void) {
    cwist_https_connection *c = cwist_alloc(sizeof(*c));
    assert(c != NULL);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    return c;
}

static void blocker(cwist_https_connection *conn, void *ctx) {
    (void)conn;
    (void)ctx;
    atomic_store(&g_blocker_running, true);
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate_open) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
}

static void record(cwist_https_connection *conn, void *ctx) {
    (void)conn;
    long n = atomic_fetch_add(&g_seen_n, 1);
    assert(n < TASKS);
    g_seen[n] = (long)(intptr_t)ctx;
    atomic_fetch_add(&g_done_n, 1);
}

static void submit_round(long base, long count) {
    for (long i = 0; i < count; i++) {
        https_pool_submit_conn(fake_conn(), NULL, record, (void *)(intptr_t)(base + i));
    }
}

int main(void) {
    setenv("CWIST_WORKER_THREADS", "1", 1);
    assert(https_pool_init() == 0);

    /* Wrap the ring at its initial size first: a few small batches that the
     * single worker drains between submits move head/tail off slot 0. */
    for (int r = 0; r < 3; r++) {
        long before = atomic_load(&g_done_n);
        submit_round(before, 700);
        while (atomic_load(&g_done_n) < before + 700) usleep(1000);
    }
    long base = atomic_load(&g_done_n);

    /* Hold the only worker, then queue far past the initial capacity. */
    https_pool_submit_conn(fake_conn(), NULL, blocker, NULL);
    while (!atomic_load(&g_blocker_running)) usleep(1000);
    submit_round(base, TASKS - base);

    pthread_mutex_lock(&g_gate_mu);
    g_gate_open = true;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
    while (atomic_load(&g_done_n) < TASKS) usleep(1000);

    for (long i = 0; i < TASKS; i++) {
        if (g_seen[i] != i) {
            fprintf(stderr, "task %ld ran as #%ld\n", g_seen[i], i);
            return 1;
        }
    }
    https_pool_destroy();
    printf("test_https_pool_queue: %d tasks ran once each, in order\n", TASKS);
    return 0;
}
