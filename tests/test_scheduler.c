#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/job/scheduler.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

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

    cwist_scheduler_destroy(s);

    test_cancel_pending_calls_drop_cb();
    test_cancel_pending_null_drop_cb();
    test_cancel_pending_null_scheduler();

    printf("All scheduler tests passed.\n");
    return 0;
}
