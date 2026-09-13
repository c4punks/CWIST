#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Correctness test for CWIST_REACTOR_DRAIN_CHUNK (issue #25, "cooperative
 * queuing"): a foreign-thread cwist_reactor_post() must not wait behind an
 * entire big CQE batch when drain-chunking is enabled, and legacy (unset)
 * behavior must be unchanged. See tests/bench_cooperative_queuing.c for the
 * full before/after latency measurement this is the pass/fail twin of.
 *
 * Same technique as test_reactor_wake.c: include the reactor implementation
 * directly to drive it without a public test API.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) { return calloc(1, size); }
void cwist_free(void *ptr) { free(ptr); }
atomic_int g_cwist_running = 1;

enum { BATCH = 256, FAKE_HANDLER_US = 100 };

static cwist_reactor_t *loop;
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
    cwist_reactor_stop(loop);
}

static void *poster_thread(void *unused) {
    (void)unused;
    usleep(200);
    clock_gettime(CLOCK_MONOTONIC, &post_sent_at);
    post_node.cb = post_cb;
    assert(cwist_reactor_post(loop, &post_node));
    return NULL;
}

static void conn_cb(int fd, void *ctx) {
    (void)ctx;
    char buf[8];
    ssize_t rc = read(fd, buf, sizeof(buf));
    (void)rc;
    spin_us(FAKE_HANDLER_US);
    handled++;
}

/* Returns the post's own post-to-callback latency in ms. */
static double run_batch(void) {
    handled = 0;
    post_latency_ms = -1.0;
    loop = cwist_reactor_create();
    assert(loop);
    if (loop->impl.use_epoll) {
        cwist_reactor_destroy(loop);
        return -1.0; /* caller skips: this mechanism is io_uring-specific */
    }

    for (int i = 0; i < BATCH; i++) {
        assert(pipe(pipes[i]) == 0);
        assert(cwist_reactor_add(loop, pipes[i][0], conn_cb, NULL, 0));
    }
    for (int i = 0; i < BATCH; i++) {
        char b = 'x';
        assert(write(pipes[i][1], &b, 1) == 1);
    }

    pthread_t poster;
    assert(pthread_create(&poster, NULL, poster_thread, NULL) == 0);
    cwist_reactor_run(loop);
    assert(pthread_join(poster, NULL) == 0);
    assert(handled == BATCH);
    assert(post_latency_ms >= 0.0);

    for (int i = 0; i < BATCH; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    cwist_reactor_destroy(loop);
    return post_latency_ms;
}

/* reactor_drain_chunk() caches its getenv() read for the life of the
 * process (same pattern as the other env knobs in reactor.c), so the two
 * configurations under test must run in separate processes -- a single
 * process changing the env var between calls would silently keep reusing
 * the first cached value. */
static double run_batch_in_child(const char *drain_chunk_env) {
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        if (drain_chunk_env) setenv("CWIST_REACTOR_DRAIN_CHUNK", drain_chunk_env, 1);
        else unsetenv("CWIST_REACTOR_DRAIN_CHUNK");
        double ms = run_batch();
        ssize_t w = write(fds[1], &ms, sizeof(ms));
        (void)w;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    double ms = -1.0;
    ssize_t r = read(fds[0], &ms, sizeof(ms));
    close(fds[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(r == (ssize_t)sizeof(ms));
    return ms;
}

int main(void) {
    double whole_batch_ms = (BATCH * FAKE_HANDLER_US) / 1000.0;

    /* Legacy (unset/0): a foreign post posted mid-batch is not expected to
     * beat the batch -- it is documented, current behavior, not the bug
     * under test here. Just confirm it completes at all (no hang/crash). */
    double legacy_ms = run_batch_in_child(NULL);
    if (legacy_ms < 0.0) {
        printf("SKIP: io_uring unavailable on this host\n");
        return 0;
    }
    printf("legacy (unset): post latency = %.3fms (whole batch ~%.3fms)\n",
           legacy_ms, whole_batch_ms);

    /* Chunked: the post must be serviced well before the whole batch would
     * have finished draining -- this is the actual fix under test. Half the
     * unchunked batch time is a generous bound (measured improvement in
     * practice is over an order of magnitude, see bench_cooperative_queuing). */
    double chunked_ms = run_batch_in_child("8");
    printf("chunk=8: post latency = %.3fms\n", chunked_ms);
    assert(chunked_ms < whole_batch_ms / 2.0);
    assert(chunked_ms < legacy_ms / 2.0);

    printf("cooperative queuing (CWIST_REACTOR_DRAIN_CHUNK) test passed\n");
    return 0;
}
