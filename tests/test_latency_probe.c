#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Standalone latency-probe contract test (issue #166). Includes the reactor
 * implementation directly (same style as test_reactor_wake.c) so the test can
 * read the per-reactor probe recorder after stopping the loop and before
 * destroy prints it. Run with CWIST_LATENCY_PROBE=1 to exercise the enabled
 * path (histograms must accumulate samples); run without it to prove the
 * disabled path is unaffected (all counters stay zero).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) {
    return calloc(1, size);
}
void cwist_free(void *ptr) {
    free(ptr);
}
atomic_int g_cwist_running = 1;

#ifdef __linux__
enum { ROUNDS = 100 };

static cwist_reactor_t *loop;
static int rounds_done;

static void *run_thread(void *arg) {
    cwist_reactor_run(arg);
    return NULL;
}

static void ping_cb(int fd, void *ctx) {
    (void)ctx;
    uint64_t one;
    ssize_t n = read(fd, &one, sizeof(one));
    (void)n;
    rounds_done++;
    if (rounds_done < ROUNDS) {
        /* Re-arm from inside the dispatch, like reactor_wake_cb does; the
         * run thread takes the deferred-SQE path. */
        assert(cwist_reactor_add(loop, fd, ping_cb, NULL, 0));
    } else {
        cwist_reactor_stop(loop);
    }
}
#endif

int main(void) {
#ifdef __linux__
    alarm(30); /* Watchdog only, not the correctness oracle. */
    bool probe_on = getenv("CWIST_LATENCY_PROBE") != NULL;
    loop = cwist_reactor_create();
    assert(loop != NULL);
    if (loop->impl.use_epoll) {
        puts("backend=epoll (skipped: probe records on the io_uring path only)");
        cwist_reactor_destroy(loop);
        return 0;
    }
    puts("backend=io_uring");

    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);
    assert(cwist_reactor_add(loop, fd, ping_cb, NULL, 0));

    pthread_t owner;
    assert(pthread_create(&owner, NULL, run_thread, loop) == 0);

    for (int i = 0; i < ROUNDS; i++) {
        uint64_t one = 1;
        ssize_t n;
        do {
            n = write(fd, &one, sizeof(one));
        } while (n < 0 && errno == EAGAIN);
        assert(n == (ssize_t)sizeof(one));
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert(pthread_join(owner, NULL) == 0);
    assert(rounds_done == ROUNDS);

    if (probe_on) {
        assert(loop->probe[LATENCY_PROBE_QUEUE].count > 0);
        assert(loop->probe[LATENCY_PROBE_SVC].count > 0);
        assert(loop->probe[LATENCY_PROBE_QUEUE].count >= (uint64_t)ROUNDS);
        /* max is monotonic per sample and cannot exceed the running sum. */
        assert(loop->probe[LATENCY_PROBE_QUEUE].max_us <= loop->probe[LATENCY_PROBE_QUEUE].sum_us);
        printf("queue_delay: count=%llu max_us=%llu\n",
               (unsigned long long)loop->probe[LATENCY_PROBE_QUEUE].count,
               (unsigned long long)loop->probe[LATENCY_PROBE_QUEUE].max_us);
        printf("svc:         count=%llu max_us=%llu\n",
               (unsigned long long)loop->probe[LATENCY_PROBE_SVC].count,
               (unsigned long long)loop->probe[LATENCY_PROBE_SVC].max_us);
    } else {
        assert(loop->probe[LATENCY_PROBE_QUEUE].count == 0);
        assert(loop->probe[LATENCY_PROBE_SVC].count == 0);
        puts("probe disabled: recorder untouched");
    }
    /* destroy() prints the [latency-probe] dump on stderr when enabled. */
    cwist_reactor_destroy(loop);
    close(fd);
    printf("latency probe %s: PASS\n", probe_on ? "enabled" : "disabled");
    return 0;
#else
    puts("latency probe test: skipped (Linux io_uring only)");
    return 0;
#endif
}
