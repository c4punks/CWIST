/**
 * @file test_https_pool_queue.c
 * @brief The HTTPS pool's task ring grows on demand and keeps FIFO order.
 *
 * One pool thread is held inside a handler while thousands of tasks are
 * queued behind it, forcing the ring through several doublings, including
 * growth while the ring has wrapped. Tasks carry a sequence number in
 * user_ctx; every one must run exactly once, in submission order. The
 * connections are inert stand-ins (no fd, no SSL), so teardown is cheap.
 *
 * A second phase has several threads submit at once while the worker is
 * held, so concurrent submitters race to grow the ring. The ring is grown
 * outside the pool mutex by one submitter at a time; every task must still
 * run once, and each submitter's tasks in its own order.
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

/* --- phase 2: concurrent submitters ------------------------------------- */

#define SUBMITTERS 8
#define PER_SUBMITTER 4000

static pthread_mutex_t g_gate2_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate2_cv = PTHREAD_COND_INITIALIZER;
static bool g_gate2_open = false;
static atomic_bool g_blocker2_running = false;
static atomic_long g_p2_done = 0;
static long g_p2_last[SUBMITTERS]; /* only the single worker writes these */
static atomic_int g_p2_errors = 0;

static void blocker2(cwist_https_connection *conn, void *ctx) {
    (void)conn;
    (void)ctx;
    atomic_store(&g_blocker2_running, true);
    pthread_mutex_lock(&g_gate2_mu);
    while (!g_gate2_open) pthread_cond_wait(&g_gate2_cv, &g_gate2_mu);
    pthread_mutex_unlock(&g_gate2_mu);
}

static void record2(cwist_https_connection *conn, void *ctx) {
    (void)conn;
    long v = (long)(intptr_t)ctx;
    long who = v / PER_SUBMITTER, seq = v % PER_SUBMITTER;
    if (who < 0 || who >= SUBMITTERS || seq != g_p2_last[who] + 1) atomic_fetch_add(&g_p2_errors, 1);
    else g_p2_last[who] = seq;
    atomic_fetch_add(&g_p2_done, 1);
}

static void *submitter(void *arg) {
    long who = (long)(intptr_t)arg;
    for (long i = 0; i < PER_SUBMITTER; i++) {
        https_pool_submit_conn(fake_conn(), NULL, record2, (void *)(intptr_t)(who * PER_SUBMITTER + i));
    }
    return NULL;
}

static void run_concurrent_submitters(void) {
    for (int i = 0; i < SUBMITTERS; i++) g_p2_last[i] = -1;
    https_pool_submit_conn(fake_conn(), NULL, blocker2, NULL);
    while (!atomic_load(&g_blocker2_running)) usleep(1000);

    pthread_t t[SUBMITTERS];
    for (long i = 0; i < SUBMITTERS; i++) assert(pthread_create(&t[i], NULL, submitter, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < SUBMITTERS; i++) pthread_join(t[i], NULL);

    pthread_mutex_lock(&g_gate2_mu);
    g_gate2_open = true;
    pthread_cond_broadcast(&g_gate2_cv);
    pthread_mutex_unlock(&g_gate2_mu);
    while (atomic_load(&g_p2_done) < SUBMITTERS * PER_SUBMITTER) usleep(1000);
    assert(atomic_load(&g_p2_errors) == 0);
    for (int i = 0; i < SUBMITTERS; i++) assert(g_p2_last[i] == PER_SUBMITTER - 1);
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
    run_concurrent_submitters();
    https_pool_destroy();
    printf("test_https_pool_queue: %d tasks ran once each, in order; %d concurrent submitters x %d ok\n",
           TASKS, SUBMITTERS, PER_SUBMITTER);
    return 0;
}
