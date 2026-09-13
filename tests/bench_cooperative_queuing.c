#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Before/after measurement for CWIST_REACTOR_DRAIN_CHUNK (issue #25,
 * "cooperative queuing" follow-up).
 *
 * Hypothesis: a big ready-batch in one io_uring wake round (the ring holds
 * up to 4096 CQEs) drains fully -- including every connection's own handler
 * work -- before cwist_reactor_post()'d foreign-thread completions (the
 * cwist_async_defer path: background DB/job continuations) are serviced.
 * A completion's own tail latency should scale with how many *other*
 * connections happened to be ready in the same wake, not with its own cost.
 *
 * This harness builds that exact shape directly against the reactor, no
 * HTTP layer involved: BATCH pipes are all made readable at once (so a
 * single io_uring_enter drains all BATCH CQEs in one round), each callback
 * doing FAKE_HANDLER_US of busy-work to stand in for a real request handler.
 * A background thread posts one foreign-thread completion partway through
 * the batch and its post-to-callback latency is measured -- with and without
 * CWIST_REACTOR_DRAIN_CHUNK set.
 *
 * Include the implementation directly (same technique as
 * tests/test_reactor_wake.c) to drive it without a public test API.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) { return calloc(1, size); }
void cwist_free(void *ptr) { free(ptr); }
atomic_int g_cwist_running = 1;

enum { BATCH = 512, FAKE_HANDLER_US = 80, POST_AFTER = 64 };

static cwist_reactor_t *loop;
static pthread_t owner;
static int pipes[BATCH][2];
static int handled;
static struct timespec post_sent_at;
static double post_latency_ms = -1.0;
static cwist_reactor_post_t post_node;

static void spin_us(int us) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000L +
             (now.tv_nsec - start.tv_nsec) / 1000L < us);
}

static double ms_between(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

static void post_cb(void *ctx) {
    (void)ctx;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    post_latency_ms = ms_between(post_sent_at, now);
    /* Foreign-thread posts are only drained at the top of an outer-loop
     * round (see reactor_drain_posts() call sites), never mid-CQE-batch in
     * legacy mode -- so this must be what stops the loop. Stopping from
     * conn_cb once handled==BATCH would exit before the *next* round's
     * top-of-loop drain ever runs, and the post would never fire at all. */
    cwist_reactor_stop(loop);
}

static void *poster_thread(void *unused) {
    (void)unused;
    /* Give the drain loop a head start into the batch, then post. */
    usleep(200);
    clock_gettime(CLOCK_MONOTONIC, &post_sent_at);
    post_node.cb = post_cb;
    assert(cwist_reactor_post(loop, &post_node));
    return NULL;
}

static void conn_cb(int fd, void *ctx) {
    (void)ctx;
    char buf[8];
    ssize_t rc = read(fd, buf, sizeof(buf)); /* exactly one byte is ever written */
    (void)rc;
    spin_us(FAKE_HANDLER_US);
    handled++;
    /* Do not stop here: see post_cb for why the loop must keep running
     * into the next round for the foreign-thread post to ever be drained. */
}

static double run_once(void) {
    handled = 0;
    post_latency_ms = -1.0;
    loop = cwist_reactor_create();
    assert(loop);
    if (loop->impl.use_epoll) {
        fprintf(stderr, "SKIP: io_uring unavailable, falling back to epoll -- "
                        "this experiment is io_uring-batch-specific\n");
        cwist_reactor_destroy(loop);
        exit(0);
    }
    owner = pthread_self();

    for (int i = 0; i < BATCH; i++) {
        assert(pipe(pipes[i]) == 0);
        assert(cwist_reactor_add(loop, pipes[i][0], conn_cb, NULL, 0));
    }
    /* Make every pipe readable before the reactor ever waits, so they all
     * land as CQEs in the same drain round. */
    for (int i = 0; i < BATCH; i++) {
        char b = 'x';
        assert(write(pipes[i][1], &b, 1) == 1);
    }

    pthread_t poster;
    assert(pthread_create(&poster, NULL, poster_thread, NULL) == 0);

    cwist_reactor_run(loop);

    assert(pthread_join(poster, NULL) == 0);
    assert(handled == BATCH);
    double result = post_latency_ms;

    for (int i = 0; i < BATCH; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    cwist_reactor_destroy(loop);
    return result;
}

int main(void) {
    /* POST_AFTER exists only to document the intended stagger; the poster
     * thread's own usleep(200) is what actually lands the post mid-batch
     * given FAKE_HANDLER_US * BATCH's total duration. */
    (void)POST_AFTER;

    const char *chunk_env = getenv("CWIST_REACTOR_DRAIN_CHUNK");
    printf("CWIST_REACTOR_DRAIN_CHUNK=%s BATCH=%d FAKE_HANDLER_US=%d\n",
           chunk_env ? chunk_env : "(unset, legacy)", BATCH, FAKE_HANDLER_US);

    enum { RUNS = 10 };
    double samples[RUNS];
    for (int i = 0; i < RUNS; i++) {
        samples[i] = run_once();
        printf("run %d: post-to-callback = %.3f ms\n", i, samples[i]);
    }
    double sum = 0, max = 0;
    for (int i = 0; i < RUNS; i++) {
        sum += samples[i];
        if (samples[i] > max) max = samples[i];
    }
    printf("mean=%.3fms max=%.3fms (expected worst case without chunking: "
           "~%.3fms = BATCH*FAKE_HANDLER_US)\n",
           sum / RUNS, max, (BATCH * FAKE_HANDLER_US) / 1000.0);
    return 0;
}
