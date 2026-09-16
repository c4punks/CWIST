/* bench_malloc_intercept_concurrent.c — per-call overhead of
 * cwist_alloc/cwist_free with full-GC mode on vs off (issue #65).
 *
 * Distinct from tests/bench_malloc_intercept.c (also issue #65): that one
 * measures the CWIST_INTERCEPT_MALLOC header shim's overhead by calling
 * malloc()/free() directly; this one calls cwist_alloc()/cwist_free()
 * directly and adds a concurrency dimension, to answer the question the
 * other file doesn't: does the pending-sweep list in gc.c become a
 * contention bottleneck under concurrent load?
 *
 * Measures three configurations:
 *   baseline  — plain malloc/free, no CWIST shim
 *   shim-off  — cwist_alloc/cwist_free, full_gc disabled
 *   shim-on   — cwist_alloc/cwist_free, full_gc enabled
 *
 * And two concurrency shapes (single-threaded and multi-threaded) to see
 * whether the pending-sweep list in gc.c becomes a contention bottleneck
 * under concurrent load -- step 2 of the suggested profiling plan in #65.
 *
 * Build:
 *   cc -O2 -std=c17 -pthread \
 *       -I include \
 *       -I lib/libttak/include \
 *       tests/bench_malloc_intercept_concurrent.c \
 *       src/core/mem/alloc.c src/core/mem/gc.c src/core/mem/arena.c \
 *       -L lib/libttak -lttak -o bench_malloc_intercept_concurrent
 *
 * Run:
 *   ./bench_malloc_intercept_concurrent
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>

/* ---------- timing ---------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- allocation mix ----------
 * Mixed sizes loosely representative of a real handler: small metadata
 * structs (16–64 bytes), mid-sized buffers (128–512 bytes), and the
 * occasional realloc. */

enum {
    ITERS = 1000000,  /* allocations per thread per run */
    NUM_THREADS = 8,       /* concurrent threads for the MT run */
    REALLOC_EVERY = 20,    /* do a realloc every N iterations */
};

static const size_t SIZES[] = {16, 24, 32, 48, 64, 96, 128, 256, 512};
#define NSIZE (sizeof(SIZES) / sizeof(SIZES[0]))

/* Single-threaded baseline: plain malloc/free */
static double run_baseline_st(void) {
    uint64_t t0 = now_ns();
    for (int i = 0; i < ITERS; i++) {
        size_t sz = SIZES[i % NSIZE];
        void *p = malloc(sz);
        if (!p) abort();
        if (i % REALLOC_EVERY == 0) {
            void *p2 = realloc(p, sz * 2);
            if (!p2) {
                free(p);
                continue;
            }
            p = p2;
        }
        free(p);
    }
    return (double)(now_ns() - t0) / ITERS;
}

/* Single-threaded cwist_alloc/cwist_free */
static double run_cwist_st(void) {
    uint64_t t0 = now_ns();
    for (int i = 0; i < ITERS; i++) {
        size_t sz = SIZES[i % NSIZE];
        void *p = cwist_alloc(sz);
        if (!p) abort();
        if (i % REALLOC_EVERY == 0) {
            void *p2 = cwist_realloc(p, sz * 2);
            if (p2) p = p2;
        }
        cwist_free(p);
    }
    return (double)(now_ns() - t0) / ITERS;
}

/* ---------- multi-threaded variant ---------- */

typedef struct {
    double ns_per_op;
} thread_result_t;

static void *thread_fn(void *arg) {
    thread_result_t *r = arg;
    r->ns_per_op = run_cwist_st();
    return NULL;
}

static double run_cwist_mt(int nthreads) {
    pthread_t threads[NUM_THREADS];
    thread_result_t results[NUM_THREADS];
    memset(results, 0, sizeof(results));

    int n = nthreads < NUM_THREADS ? nthreads : NUM_THREADS;
    for (int i = 0; i < n; i++) pthread_create(&threads[i], NULL, thread_fn, &results[i]);
    for (int i = 0; i < n; i++) pthread_join(threads[i], NULL);

    /* report the average ns/op across threads — each thread does ITERS ops */
    double total = 0.0;
    for (int i = 0; i < n; i++) total += results[i].ns_per_op;
    return total / n;
}

/* ---------- main ---------- */

static void print_row(const char *label, double ns_per_op, double baseline) {
    double overhead_pct = (ns_per_op - baseline) / baseline * 100.0;
    printf("  %-40s  %6.1f ns/op   %+5.0f%%\n", label, ns_per_op, overhead_pct);
}

int main(void) {
    puts("bench_malloc_intercept — cwist_alloc full-GC overhead (issue #65)");
    puts("====================================================================");
    printf("  iters per thread: %d    threads (MT run): %d\n\n", ITERS, NUM_THREADS);

    /* --- single-threaded --- */
    puts("Single-threaded:");

    double baseline_ns = run_baseline_st();
    printf("  %-40s  %6.1f ns/op  (baseline)\n", "plain malloc/free", baseline_ns);

    /* shim on / full_gc off */
    double st_off = run_cwist_st();
    print_row("cwist_alloc/free  full_gc=off", st_off, baseline_ns);

    /* shim on / full_gc on */
    cwist_full_gc(true);
    double st_on = run_cwist_st();
    print_row("cwist_alloc/free  full_gc=on", st_on, baseline_ns);

    puts("");

    /* --- multi-threaded (full_gc still on from above) --- */
    printf("Multi-threaded (%d threads, full_gc=on):\n", NUM_THREADS);
    double mt_on = run_cwist_mt(NUM_THREADS);
    print_row("cwist_alloc/free  full_gc=on", mt_on, baseline_ns);

    puts("");
    puts("Interpretation guide:");
    puts("  full_gc=off overhead  should be ~3% (one relaxed atomic load).");
    puts("  full_gc=on  overhead  tracks the pending-sweep list cost.");
    puts("  MT vs ST gap         reveals per-thread vs shared-list contention.");
    puts("  See issue #65 for the original single-threaded measurements and");
    puts("  suggested next steps (perf profiling, contention analysis).");

    return 0;
}
