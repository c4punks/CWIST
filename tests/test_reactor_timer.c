#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Reactor one-shot timer test.  Includes the implementation like
 * test_reactor_wake.c so it runs without the rest of libcwist; run it once per
 * backend (CWIST_REACTOR_BACKEND=epoll forces epoll on Linux).
 *
 * Checks: timers fire in deadline order regardless of arm order, never early;
 * a cancelled timer never fires; re-arming replaces the deadline; a callback
 * can re-arm itself.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) {
    return calloc(1, size);
}
void cwist_free(void *ptr) {
    free(ptr);
}
atomic_int g_cwist_running = 1;

static cwist_reactor_t *loop;
static uint64_t start_ns;

typedef struct {
    cwist_reactor_timer_t timer;
    uint64_t delay_us;
    uint64_t fired_ns;
    int order;
} probe_t;

static int fired_count;
static probe_t probes[5];
static cwist_reactor_timer_t cancelled, rearmed, repeat, stopper;
static int cancelled_fired, rearmed_fired, repeat_left = 5;
static uint64_t rearmed_fired_ns;

static void probe_cb(void *ctx) {
    probe_t *p = ctx;
    p->fired_ns = reactor_now_ns();
    p->order = fired_count++;
}

static void cancelled_cb(void *ctx) {
    (void)ctx;
    cancelled_fired = 1;
}

static void rearmed_cb(void *ctx) {
    (void)ctx;
    rearmed_fired++;
    rearmed_fired_ns = reactor_now_ns();
}

static void repeat_cb(void *ctx) {
    (void)ctx;
    if (--repeat_left > 0) assert(cwist_reactor_timer_arm(loop, &repeat, 2000));
}

static void stop_cb(void *ctx) {
    (void)ctx;
    cwist_reactor_stop(loop);
}

static void *run_loop(void *arg) {
    (void)arg;
    cwist_reactor_run(loop);
    return NULL;
}

int main(void) {
    loop = cwist_reactor_create();
    assert(loop);
    start_ns = reactor_now_ns();

    /* Arm out of deadline order. */
    const uint64_t delays_us[5] = {40000, 10000, 30000, 5000, 20000};
    for (int i = 0; i < 5; i++) {
        probes[i].delay_us = delays_us[i];
        cwist_reactor_timer_init(&probes[i].timer, probe_cb, &probes[i]);
        assert(cwist_reactor_timer_arm(loop, &probes[i].timer, delays_us[i]));
    }
    cwist_reactor_timer_init(&cancelled, cancelled_cb, NULL);
    assert(cwist_reactor_timer_arm(loop, &cancelled, 15000));
    cwist_reactor_timer_cancel(loop, &cancelled);
    assert(!cwist_reactor_timer_armed(&cancelled));

    /* Re-arm pushes the deadline out: must fire once, near 50 ms. */
    cwist_reactor_timer_init(&rearmed, rearmed_cb, NULL);
    assert(cwist_reactor_timer_arm(loop, &rearmed, 1000));
    assert(cwist_reactor_timer_arm(loop, &rearmed, 50000));

    cwist_reactor_timer_init(&repeat, repeat_cb, NULL);
    assert(cwist_reactor_timer_arm(loop, &repeat, 2000));

    cwist_reactor_timer_init(&stopper, stop_cb, NULL);
    assert(cwist_reactor_timer_arm(loop, &stopper, 80000));

    pthread_t th;
    assert(pthread_create(&th, NULL, run_loop, NULL) == 0);
    assert(pthread_join(th, NULL) == 0);

    assert(fired_count == 5);
    for (int i = 0; i < 5; i++) {
        uint64_t elapsed_us = (probes[i].fired_ns - start_ns) / 1000;
        assert(elapsed_us >= probes[i].delay_us);
        /* Generous lateness bound for loaded CI runners. */
        assert(elapsed_us < probes[i].delay_us + 50000);
        for (int j = 0; j < 5; j++) {
            if (probes[j].delay_us < probes[i].delay_us) assert(probes[j].order < probes[i].order);
        }
    }
    assert(!cancelled_fired);
    assert(rearmed_fired == 1);
    assert((rearmed_fired_ns - start_ns) / 1000 >= 50000);
    assert(repeat_left == 0);
    assert(loop->timer_n == 0);

    cwist_reactor_destroy(loop);
    printf("test_reactor_timer: all assertions passed (%s)\n",
#ifdef __linux__
           getenv("CWIST_REACTOR_BACKEND") ? getenv("CWIST_REACTOR_BACKEND") : "default"
#else
           "kqueue"
#endif
    );
    return 0;
}
