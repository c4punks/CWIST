/* bench_full_gc_tracking.c - cost of full-GC's per-thread pending-sweep
 * bookkeeping (issue #65), as a function of how many tracked blocks are
 * live, for tracked and untracked frees, and across threads.
 *
 * tests/bench_malloc_intercept.c frees each block right after allocating
 * it, so the pending list never holds more than a block or two. Real
 * handlers keep blocks alive (request/response structs, headers, parsed
 * JSON) while they allocate more, so this bench first parks N live tracked
 * blocks on the thread and then measures the churn on top of them:
 *
 *   churn     cwist_alloc() + cwist_free() of a fresh block (tracked:
 *             one track + one untrack that finds it)
 *   untracked cwist_strdup() + cwist_free() (cwist_strdup() is never
 *             tracked, so cwist_free()'s untrack is a miss)
 *   threads   churn on 1/2/4/8 threads at once, each with its own live set
 *
 * Usage: ./bench_full_gc_tracking on|off [iterations]
 * Full-GC is a process-wide latch, so "on" and "off" are separate runs.
 * Manual probe; not part of `make test` (see the Makefile target).
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long g_iters = 200000;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void **park_live(size_t n) {
    void **live = (void **)calloc(n ? n : 1, sizeof(void *));
    if (!live) exit(1);
    for (size_t i = 0; i < n; i++) {
        live[i] = cwist_alloc(64);
        if (!live[i]) exit(1);
    }
    return live;
}

static void release_live(void **live, size_t n) {
    for (size_t i = 0; i < n; i++) cwist_free(live[i]);
    free(live);
}

/* ns per alloc+free pair with @p live_n tracked blocks parked. */
static double churn(size_t live_n, long iters) {
    void **live = park_live(live_n);
    double t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        void *p = cwist_alloc(16 + (size_t)(i & 7) * 16);
        if (!p) exit(1);
        cwist_free(p);
    }
    double ns = (now_ns() - t0) / (double)iters;
    release_live(live, live_n);
    return ns;
}

/* ns per strdup+free pair (untracked free) with @p live_n blocks parked. */
static double untracked(size_t live_n, long iters) {
    void **live = park_live(live_n);
    double t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        char *s = cwist_strdup("x-request-id");
        if (!s) exit(1);
        cwist_free(s);
    }
    double ns = (now_ns() - t0) / (double)iters;
    release_live(live, live_n);
    return ns;
}

typedef struct {
    size_t live_n;
    double ns;
} thread_arg;

static void *thread_churn(void *arg) {
    thread_arg *a = (thread_arg *)arg;
    a->ns = churn(a->live_n, g_iters);
    return NULL;
}

/* Mean per-thread ns per pair with @p nthreads churning concurrently. */
static double threaded(int nthreads, size_t live_n) {
    pthread_t tid[8];
    thread_arg args[8];
    for (int i = 0; i < nthreads; i++) {
        args[i].live_n = live_n;
        args[i].ns = 0;
        if (pthread_create(&tid[i], NULL, thread_churn, &args[i]) != 0) exit(1);
    }
    double sum = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tid[i], NULL);
        sum += args[i].ns;
    }
    return sum / nthreads;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "on";
    if (argc > 2) g_iters = atol(argv[2]);
    if (strcmp(mode, "on") == 0) cwist_full_gc(true);
    printf("full-GC %s, %ld iterations per measurement\n", cwist_full_gc_enabled() ? "on" : "off",
           g_iters);

    static const size_t live_sizes[] = {0, 64, 1024, 16384};
    for (size_t i = 0; i < sizeof(live_sizes) / sizeof(live_sizes[0]); i++) {
        size_t n = live_sizes[i];
        printf("  live=%-6zu churn %8.1f ns/pair   untracked free %8.1f ns/pair\n", n,
               churn(n, g_iters), untracked(n, g_iters));
    }
    static const int thread_counts[] = {1, 2, 4, 8};
    for (size_t i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]); i++) {
        printf("  threads=%d live=1024/thread churn %8.1f ns/pair\n", thread_counts[i],
               threaded(thread_counts[i], 1024));
    }
    if (cwist_full_gc_enabled()) cwist_gc_scope_flush();
    return 0;
}
