#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Tail-elevation heuristic (CWIST_REACTOR_TAIL_US): an event whose arm stamp
 * is older than the threshold is served before younger events of the same
 * batch and its callback observes cwist_reactor_dispatch_late() == true;
 * below the threshold, with the knob at 0, and outside dispatch it is false.
 * Both reactor backends.
 *
 * Same technique as test_reactor_late.c: include the reactor implementation
 * directly, and run each configuration in its own process because the knob
 * is cached on first read.
 *
 * Two mechanisms are exercised:
 *   - the round-start scan pulls an already-aged event to the front of the
 *     batch (arm one pipe, sleep past the threshold, arm the rest, run);
 *   - the delivery-time check catches events that age behind a slow earlier
 *     callback (first callback spins past the threshold).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) {
    return calloc(1, size);
}
void cwist_free(void *ptr) {
    free(ptr);
}
atomic_int g_cwist_running = 1;

enum { BATCH = 8 };

static cwist_reactor_t *loop;
static int pipes[BATCH][2];
static int order[BATCH];
static int late_seen[BATCH];
static int handled;
static int spin_first_us;
static int bypass_in_first; /* model the HTTP RX handler raising the bypass */

static void spin_us(long us) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000L + (now.tv_nsec - start.tv_nsec) / 1000L < us);
}

static void conn_cb(int fd, void *ctx) {
    (void)fd;
    int idx = *(int *)ctx;
    char buf[8];
    ssize_t rc = read(fd, buf, sizeof(buf));
    (void)rc;
    /* Model the HTTP RX handler: it may raise the bypass (its event aged on
     * the HTTP side) before running the serve path that reads
     * cwist_reactor_dispatch_late(). */
    if (handled == 0 && bypass_in_first) cwist_reactor_dispatch_age_bypass();
    if (handled == 0 && spin_first_us > 0) spin_us(spin_first_us);
    order[handled] = idx;
    late_seen[handled] = cwist_reactor_dispatch_late();
    handled++;
    if (handled == BATCH) cwist_reactor_stop(loop);
}

typedef struct {
    int skipped; /* -1: backend unavailable */
    int order[BATCH];
    int late[BATCH];
    unsigned long long elevations;
} result_t;

static void sleep_us(long us) {
    struct timespec ts = {.tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

/* Every pipe is readable before its arm, so all callbacks land in a single
 * dispatch round. tail_env selects the knob (NULL = default); when
 * sleep_before_rest is set, pipe 0 is armed first and the rest after a 2 ms
 * sleep (pipe 0 is aged at the round-start scan, the rest are young). */
static result_t run_batch(const char *tail_env, const char *backend_env, bool sleep_before_rest,
                          int spin_us_first, bool bypass_first) {
    result_t res;
    memset(&res, 0, sizeof(res));
    handled = 0;
    spin_first_us = spin_us_first;
    bypass_in_first = bypass_first ? 1 : 0;
    memset(order, 0, sizeof(order));
    memset(late_seen, 0, sizeof(late_seen));
    if (tail_env) setenv("CWIST_REACTOR_TAIL_US", tail_env, 1);
    if (backend_env) setenv("CWIST_REACTOR_BACKEND", backend_env, 1);
    setenv("CWIST_REACTOR_LATE_US", "0", 1); /* isolate from late-round mode */

    loop = cwist_reactor_create();
    assert(loop);
    /* A kernel without io_uring support cannot run the uring backend. */
    if (!backend_env && loop->impl.use_epoll) {
        cwist_reactor_destroy(loop);
        res.skipped = -1;
        return res;
    }
    for (int i = 0; i < BATCH; i++) {
        assert(pipe(pipes[i]) == 0);
        char b = 'x';
        assert(write(pipes[i][1], &b, 1) == 1);
    }
    assert(cwist_reactor_add(loop, pipes[0][0], conn_cb, &*(int[]){0}, sizeof(int)));
    if (sleep_before_rest) sleep_us(2000); /* > the 1000 us test threshold */
    for (int i = 1; i < BATCH; i++) {
        assert(cwist_reactor_add(loop, pipes[i][0], conn_cb, &*(int[]){i}, sizeof(int)));
    }
    cwist_reactor_run(loop);
    assert(handled == BATCH);
    memcpy(res.order, order, sizeof(order));
    memcpy(res.late, late_seen, sizeof(late_seen));
    res.elevations = (unsigned long long)loop->tail_elevations;
    for (int i = 0; i < BATCH; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    cwist_reactor_destroy(loop);
    return res;
}

static result_t run_in_child(const char *tail_env, const char *backend_env,
                             bool sleep_before_rest, int spin_us_first, bool bypass_first) {
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        result_t r =
            run_batch(tail_env, backend_env, sleep_before_rest, spin_us_first, bypass_first);
        ssize_t w = write(fds[1], &r, sizeof(r));
        (void)w;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    result_t r;
    memset(&r, 0, sizeof(r));
    r.skipped = -2;
    ssize_t n = read(fds[0], &r, sizeof(r));
    close(fds[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(n == (ssize_t)sizeof(r));
    return r;
}

/* Natural dispatch order is 0..BATCH-1 (uring: CQEs complete in submission
 * order; the asserts that need an order are gated on strict_order). */
static void expect_natural_order(const result_t *r, bool strict_order) {
    if (strict_order) {
        for (int i = 0; i < BATCH; i++) assert(r->order[i] == i);
    } else {
        assert(r->order[0] == 0); /* the aged pipe 0 must still be served first */
    }
}

int main(void) {
    /* Pure age check: threshold semantics. */
    assert(cwist_reactor_event_aged(4500, 1000, 3500));  /* age == tail */
    assert(cwist_reactor_event_aged(4501, 1000, 3500));  /* age > tail */
    assert(!cwist_reactor_event_aged(4499, 1000, 3500)); /* age < tail */
    assert(!cwist_reactor_event_aged(4500, 1000, 0));    /* disabled */
    assert(!cwist_reactor_event_aged(4500, 0, 3500));    /* unstamped */
    printf("pure age check passed\n");

    const char *backends[2] = {NULL, "epoll"};
    for (int b = 0; b < 2; b++) {
        const char *backend = backends[b];
        const bool strict = backend == NULL; /* uring CQEs complete in order */
        const char *tag = backend ? "epoll" : "uring";
        /* An ambient CWIST_REACTOR_BACKEND override is exercised by the
         * explicit epoll block below. */
        if (!backend && getenv("CWIST_REACTOR_BACKEND")) continue;

        /* Round-start scan: pipe 0 aged (armed 2 ms ago, threshold 1 ms) is
         * elevated; the rest are young both at the scan and at delivery.
         * Exactly one elevation and the scan must not double-dispatch. */
        result_t scan = run_in_child("1000", backend, true, 0, false);
        assert(scan.skipped == 0);
        assert(scan.elevations == 1);
        assert(scan.late[0] == 1);
        for (int i = 1; i < BATCH; i++) assert(scan.late[i] == 0);
        expect_natural_order(&scan, strict);
        printf("%s: round-start scan elevates the aged event\n", tag);

        /* Delivery-time check: nobody is aged at the scan; the first callback
         * spins 2 ms, so every later delivery of the same batch is aged. */
        result_t deliv = run_in_child("1000", backend, false, 2000, false);
        assert(deliv.skipped == 0);
        assert(deliv.elevations >= BATCH - 1);
        assert(deliv.late[0] == 0);
        for (int i = 1; i < BATCH; i++) assert(deliv.late[i] == 1);
        printf("%s: delivery-time check catches batch-aged events\n", tag);

        /* Default threshold (3500 us): a 2 ms stamp never qualifies. */
        result_t def = run_in_child(NULL, backend, true, 0, false);
        assert(def.skipped == 0);
        assert(def.elevations == 0);
        for (int i = 0; i < BATCH; i++) assert(def.late[i] == 0);
        expect_natural_order(&def, strict);
        printf("%s: default 3500 us threshold not crossed by 2 ms\n", tag);

        /* 60 ms threshold: neither the 2 ms scan case nor a 2 ms spin case
         * qualifies. */
        result_t high = run_in_child("60000", backend, true, 2000, false);
        assert(high.skipped == 0);
        assert(high.elevations == 0);
        for (int i = 0; i < BATCH; i++) assert(high.late[i] == 0);
        printf("%s: 60000 us threshold elevates nothing\n", tag);

        /* 0 disables the heuristic entirely. */
        result_t off = run_in_child("0", backend, true, 2000, false);
        assert(off.skipped == 0);
        assert(off.elevations == 0);
        for (int i = 0; i < BATCH; i++) assert(off.late[i] == 0);
        printf("%s: 0 disables\n", tag);

        /* Bypass scoping: a handler that raises the bypass (the HTTP RX
         * completion does) affects only its own callback; with the flag
         * leaking into the rest of the batch the later late[] entries below
         * would flip to 1. */
        result_t scope = run_in_child("60000", backend, false, 0, true);
        assert(scope.skipped == 0);
        assert(scope.elevations == 0);
        assert(scope.late[0] == 1);
        for (int i = 1; i < BATCH; i++) assert(scope.late[i] == 0);
        printf("%s: bypass is scoped to its own callback\n", tag);
    }

    printf("tail-elevation heuristic (CWIST_REACTOR_TAIL_US) test passed\n");
    return 0;
}
