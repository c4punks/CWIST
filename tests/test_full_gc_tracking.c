#define _POSIX_C_SOURCE 200809L
/* Full-GC pending-set bookkeeping (issue #65): cwist_alloc()/cwist_free()
 * keep a per-thread set of tracked blocks. This checks the set stays exact
 * through large live sets, frees in arbitrary order, frees of untracked
 * blocks, disown, realloc, flush, several threads at once, and allocations
 * made while a thread is being torn down. The sanitizer build turns any
 * lost or doubly retired block into a leak or double free. */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LIVE 20000

static void shuffle(void **a, size_t n, unsigned *seed) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand_r(seed) % (i + 1);
        void *t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static void test_large_live_set(void) {
    size_t base = cwist_gc_scope_pending_count();
    void **blocks = (void **)calloc(LIVE, sizeof(void *));
    assert(blocks);
    for (size_t i = 0; i < LIVE; i++) {
        blocks[i] = cwist_alloc(24 + (i % 5) * 8);
        assert(blocks[i]);
    }
    assert(cwist_gc_scope_pending_count() == base + LIVE);

    /* Untracked frees (cwist_strdup() is never tracked) leave the set alone,
     * even with a large set to miss against. */
    for (int i = 0; i < 1000; i++) {
        char *s = cwist_strdup("untracked");
        assert(s);
        cwist_free(s);
    }
    int stack_value = 0;
    assert(!cwist_gc_scope_untrack(&stack_value));
    assert(cwist_gc_scope_pending_count() == base + LIVE);

    /* Free in random order; every free removes exactly one entry. */
    unsigned seed = 65;
    shuffle(blocks, LIVE, &seed);
    for (size_t i = 0; i < LIVE; i++) {
        cwist_free(blocks[i]);
        assert(cwist_gc_scope_pending_count() == base + LIVE - i - 1);
    }
    free(blocks);
    printf("Passed test_large_live_set\n");
}

static void test_disown_and_realloc(void) {
    size_t base = cwist_gc_scope_pending_count();
    void *p = cwist_alloc(32);
    assert(p && cwist_gc_scope_pending_count() == base + 1);

    /* A tracked block moved by realloc stays tracked under its new address. */
    p = cwist_realloc(p, 4096);
    assert(p && cwist_gc_scope_pending_count() == base + 1);

    assert(cwist_gc_scope_disown(p));
    assert(!cwist_gc_scope_disown(p));
    assert(cwist_gc_scope_pending_count() == base);
    cwist_free(p); /* disowned blocks are still freed normally */
    assert(cwist_gc_scope_pending_count() == base);
    printf("Passed test_disown_and_realloc\n");
}

static void test_flush(void) {
    /* Enough blocks to grow the set well past its reuse limit. */
    for (int i = 0; i < 5000; i++) assert(cwist_alloc(16));
    assert(cwist_gc_scope_pending_count() >= 5000);
    cwist_gc_scope_flush();
    assert(cwist_gc_scope_pending_count() == 0);
    for (int i = 0; i < 16; i++) cwist_gc_pipeline_tick();

    /* The set keeps working after a flush dropped its table. */
    void *p = cwist_alloc(16);
    assert(cwist_gc_scope_pending_count() == 1);
    cwist_free(p);
    assert(cwist_gc_scope_pending_count() == 0);
    printf("Passed test_flush\n");
}

static void *churn_thread(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    void **blocks = (void **)calloc(2000, sizeof(void *));
    assert(blocks);
    for (int round = 0; round < 20; round++) {
        for (size_t i = 0; i < 2000; i++) assert((blocks[i] = cwist_alloc(32)));
        assert(cwist_gc_scope_pending_count() == 2000);
        shuffle(blocks, 2000, &seed);
        for (size_t i = 0; i < 2000; i++) cwist_free(blocks[i]);
        assert(cwist_gc_scope_pending_count() == 0);
    }
    free(blocks);
    /* Leave a few behind for the thread-exit sweep. */
    for (int i = 0; i < 10; i++) assert(cwist_alloc(8));
    return NULL;
}

static void test_threads(void) {
    size_t base = cwist_gc_scope_pending_count();
    pthread_t t[4];
    for (uintptr_t i = 0; i < 4; i++) {
        assert(pthread_create(&t[i], NULL, churn_thread, (void *)(i + 1)) == 0);
    }
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    /* Each thread has its own set; none of that touched this thread's. */
    assert(cwist_gc_scope_pending_count() == base);
    printf("Passed test_threads\n");
}

/* A pthread key destructor that allocates while its thread is exiting, in
 * whatever order the runtime runs it relative to full-GC's own destructor.
 * The block must still be swept (no leak) and nothing may touch a set that
 * was already freed (no use-after-free). */
static pthread_key_t late_key;

static void late_destructor(void *arg) {
    (void)arg;
    assert(cwist_alloc(48));
}

static void *late_alloc_thread(void *arg) {
    (void)arg;
    assert(cwist_alloc(16));
    pthread_setspecific(late_key, (void *)1);
    return NULL;
}

static void test_alloc_during_thread_exit(void) {
    assert(pthread_key_create(&late_key, late_destructor) == 0);
    for (int i = 0; i < 8; i++) {
        pthread_t t;
        assert(pthread_create(&t, NULL, late_alloc_thread, NULL) == 0);
        pthread_join(t, NULL);
    }
    printf("Passed test_alloc_during_thread_exit\n");
}

int main(void) {
    cwist_full_gc(true);
    assert(cwist_full_gc_enabled());
    test_large_live_set();
    test_disown_and_realloc();
    test_flush();
    test_threads();
    test_alloc_during_thread_exit();
    for (int i = 0; i < 16; i++) cwist_gc_pipeline_tick();
    printf("All full-GC tracking tests passed!\n");
    return 0;
}
