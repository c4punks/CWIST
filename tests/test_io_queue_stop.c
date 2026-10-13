#define _POSIX_C_SOURCE 200809L
/* cwist_io_queue_stop() must wake every runner, including ones blocked
 * waiting for work, and runners must finish the jobs submitted before the
 * stop. Written for the kqueue backend, whose stop used to close the
 * descriptor and hope a blocked kevent() noticed; it runs on every backend.
 */
#include <cwist/sys/io/cwist_io.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define RUNNERS 4
#define JOBS 2000

static atomic_int g_ran = 0;
static atomic_int g_exited = 0;

static void count_job(void *arg) {
    (void)arg;
    atomic_fetch_add_explicit(&g_ran, 1, memory_order_relaxed);
}

static void *run_queue(void *arg) {
    cwist_io_queue_run((cwist_io_queue *)arg);
    atomic_fetch_add_explicit(&g_exited, 1, memory_order_release);
    return NULL;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Wait up to 5s for every runner to return, so a hang fails the test
 * instead of wedging CI in pthread_join(). */
static void wait_runners_exit(void) {
    for (int i = 0; i < 500; i++) {
        if (atomic_load_explicit(&g_exited, memory_order_acquire) == RUNNERS) return;
        sleep_ms(10);
    }
    fprintf(stderr, "test_io_queue_stop: %d of %d runners still blocked after stop\n",
            RUNNERS - atomic_load(&g_exited), RUNNERS);
    assert(!"runners did not wake on stop");
}

static void start(cwist_io_queue *q, pthread_t *t) {
    atomic_store(&g_ran, 0);
    atomic_store(&g_exited, 0);
    for (int i = 0; i < RUNNERS; i++) {
        int rc = pthread_create(&t[i], NULL, run_queue, q);
        assert(rc == 0);
        (void)rc;
    }
}

static void join_all(pthread_t *t) {
    for (int i = 0; i < RUNNERS; i++) pthread_join(t[i], NULL);
}

int main(void) {
    pthread_t t[RUNNERS];

    /* 1. Idle runners, all blocked waiting for work, wake on stop. */
    cwist_io_queue *q = cwist_io_queue_create(0);
    assert(q != NULL);
    start(q, t);
    sleep_ms(50);
    cwist_io_queue_stop(q);
    wait_runners_exit();
    join_all(t);
    assert(!cwist_io_queue_submit(q, count_job, NULL));
    cwist_io_queue_destroy(q);

    /* 2. Jobs submitted before the stop all run before the runners return. */
    q = cwist_io_queue_create(0);
    assert(q != NULL);
    start(q, t);
    for (int i = 0; i < JOBS; i++) {
        bool ok = cwist_io_queue_submit(q, count_job, NULL);
        assert(ok);
        (void)ok;
    }
    cwist_io_queue_stop(q);
    wait_runners_exit();
    join_all(t);
    assert(atomic_load(&g_ran) == JOBS);
    cwist_io_queue_destroy(q);

    /* 3. Destroying a stopped queue nobody ran frees its queued jobs. */
    q = cwist_io_queue_create(0);
    assert(q != NULL);
    for (int i = 0; i < 16; i++) {
        bool ok = cwist_io_queue_submit(q, count_job, NULL);
        assert(ok);
        (void)ok;
    }
    cwist_io_queue_stop(q);
    cwist_io_queue_destroy(q);

    printf("test_io_queue_stop: ok\n");
    return 0;
}
