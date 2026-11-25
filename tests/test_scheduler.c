#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/job/scheduler.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdatomic.h>

static _Atomic int fail_alloc;
static _Atomic int alloc_entered;
static pthread_mutex_t alloc_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t alloc_gate_cv = PTHREAD_COND_INITIALIZER;

static void wait_allocation_gate(void) {
    pthread_mutex_lock(&alloc_gate);
    alloc_entered = 1;
    while (fail_alloc == 2) pthread_cond_wait(&alloc_gate_cv, &alloc_gate);
    pthread_mutex_unlock(&alloc_gate);
}

void *__real_cwist_alloc(size_t size);
void *__wrap_cwist_alloc(size_t size) {
    int expected = 1;
    if (atomic_load_explicit(&fail_alloc, memory_order_relaxed) == 1 &&
        atomic_compare_exchange_strong_explicit(&fail_alloc, &expected, 0, memory_order_relaxed,
                                                memory_order_relaxed))
        return NULL;
    if (fail_alloc == 2) wait_allocation_gate();
    return __real_cwist_alloc(size);
}

void *__real_cwist_realloc(void *ptr, size_t size);
void *__wrap_cwist_realloc(void *ptr, size_t size) {
    if (fail_alloc == 2) wait_allocation_gate();
    return __real_cwist_realloc(ptr, size);
}

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_counter = 0;

static void increment_job(void *arg) {
    int value = *(int *)arg;
    pthread_mutex_lock(&g_mutex);
    g_counter += value;
    pthread_mutex_unlock(&g_mutex);
    free(arg);
}

static void *alloc_arg(int value) {
    int *p = malloc(sizeof(int));
    *p = value;
    return p;
}

/* --- cancel_pending tests ------------------------------------------------ */

static int g_drop_count = 0;

static void counting_drop(void *arg) {
    free(arg);
    g_drop_count++;
}

static void test_cancel_pending_calls_drop_cb(void) {
    printf("test_cancel_pending_calls_drop_cb...\n");
    g_drop_count = 0;

    /* Use a very long delay so the jobs never fire during this test. */
    cwist_scheduler_t *s = cwist_scheduler_create(1, 8);
    assert(s);

    const int N = 5;
    for (int i = 0; i < N; i++) {
        assert(cwist_scheduler_schedule(s, increment_job, alloc_arg(i), 60000));
    }
    assert((int)cwist_scheduler_pending_count(s) == N);

    size_t cancelled = cwist_scheduler_cancel_pending(s, counting_drop);
    assert((int)cancelled == N);
    assert(g_drop_count == N);
    assert(cwist_scheduler_pending_count(s) == 0);

    cwist_scheduler_destroy(s);
    printf("  OK\n");
}

static void test_cancel_pending_null_drop_cb(void) {
    printf("test_cancel_pending_null_drop_cb...\n");

    cwist_scheduler_t *s = cwist_scheduler_create(1, 8);
    assert(s);

    int stack_args[3] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        assert(cwist_scheduler_schedule(s, increment_job, &stack_args[i], 60000));
    }
    size_t cancelled = cwist_scheduler_cancel_pending(s, NULL);
    assert(cancelled == 3);
    assert(cwist_scheduler_pending_count(s) == 0);

    cwist_scheduler_destroy(s);
    printf("  OK\n");
}

static void test_cancel_pending_null_scheduler(void) {
    printf("test_cancel_pending_null_scheduler...\n");
    size_t n = cwist_scheduler_cancel_pending(NULL, counting_drop);
    assert(n == 0);
    printf("  OK\n");
}

static void noop_job(void *arg) {
    (void)arg;
}

typedef struct {
    cwist_scheduler_t *scheduler;
    uint64_t delay_ms;
    bool accepted;
} schedule_call_t;

static void *schedule_call(void *arg) {
    schedule_call_t *call = arg;
    call->accepted = cwist_scheduler_schedule(call->scheduler, noop_job, NULL, call->delay_ms);
    return NULL;
}

static void *release_allocation(void *arg) {
    (void)arg;
    usleep(50000);
    pthread_mutex_lock(&alloc_gate);
    fail_alloc = 0;
    pthread_cond_broadcast(&alloc_gate_cv);
    pthread_mutex_unlock(&alloc_gate);
    return NULL;
}

static void test_schedule_destroy_race(void) {
    const uint64_t delays[] = {0, 60000};
    for (size_t i = 0; i < 2; i++) {
        cwist_scheduler_t *s = cwist_scheduler_create(1, 16);
        assert(s != NULL);
        schedule_call_t call = {.scheduler = s, .delay_ms = delays[i]};
        fail_alloc = 2;
        alloc_entered = 0;
        pthread_t caller, releaser;
        assert(pthread_create(&caller, NULL, schedule_call, &call) == 0);
        while (!alloc_entered) usleep(1000);
        assert(pthread_create(&releaser, NULL, release_allocation, NULL) == 0);
        cwist_scheduler_destroy(s);
        pthread_join(caller, NULL);
        pthread_join(releaser, NULL);
        assert(call.accepted);
    }
}

static void test_delayed_submit_allocation_retry(void) {
    cwist_scheduler_t *s = cwist_scheduler_create(1, 16);
    assert(s != NULL);
    pthread_mutex_lock(&g_mutex);
    g_counter = 0;
    pthread_mutex_unlock(&g_mutex);
    assert(cwist_scheduler_schedule(s, increment_job, alloc_arg(5), 1));
    fail_alloc = 1;
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&g_mutex);
        int count = g_counter;
        pthread_mutex_unlock(&g_mutex);
        if (count == 5) break;
        usleep(10000);
    }
    pthread_mutex_lock(&g_mutex);
    assert(g_counter == 5);
    pthread_mutex_unlock(&g_mutex);
    cwist_scheduler_destroy(s);
}

/* --- main ---------------------------------------------------------------- */

int main(void) {
    cwist_scheduler_t *s = cwist_scheduler_create(2, 16);
    assert(s != NULL);

    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(1)));
    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(2)));
    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(3)));

    /* Wait for immediate jobs to drain. */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&g_mutex);
        int c = g_counter;
        pthread_mutex_unlock(&g_mutex);
        if (c == 6) break;
        usleep(10000);
    }

    pthread_mutex_lock(&g_mutex);
    assert(g_counter == 6);
    pthread_mutex_unlock(&g_mutex);

    /* Schedule a delayed job. */
    assert(cwist_scheduler_schedule(s, increment_job, alloc_arg(10), 150));
    assert(cwist_scheduler_pending_count(s) == 1);

    usleep(300000);

    pthread_mutex_lock(&g_mutex);
    assert(g_counter == 16);
    pthread_mutex_unlock(&g_mutex);
    assert(cwist_scheduler_pending_count(s) == 0);

    int overflow_arg = 1;
    assert(!cwist_scheduler_schedule(s, increment_job, &overflow_arg, UINT64_MAX));

    cwist_scheduler_destroy(s);

    test_cancel_pending_calls_drop_cb();
    test_cancel_pending_null_drop_cb();
    test_cancel_pending_null_scheduler();
    test_delayed_submit_allocation_retry();
    test_schedule_destroy_race();

    printf("All scheduler tests passed.\n");
    return 0;
}
