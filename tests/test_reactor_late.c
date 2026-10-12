#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Late-round mode (CWIST_REACTOR_LATE_US, issue #319 experiment): once a
 * dispatch round has run past the threshold, the rest of its callbacks see
 * cwist_reactor_dispatch_late() == true; below the threshold, with the knob
 * at 0, and outside dispatch it is false.
 *
 * Same technique as test_reactor_drain_chunk.c: include the reactor
 * implementation directly, and run each configuration in its own process
 * because the knob is cached on first read.
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
static int handled;
static int late_seen;
static int spin_us_per_cb;

static void spin_us(int us) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000L + (now.tv_nsec - start.tv_nsec) / 1000L < us);
}

static void conn_cb(int fd, void *ctx) {
    (void)ctx;
    char buf[8];
    ssize_t rc = read(fd, buf, sizeof(buf));
    (void)rc;
    if (cwist_reactor_dispatch_late()) late_seen++;
    spin_us(spin_us_per_cb);
    if (++handled == BATCH) cwist_reactor_stop(loop);
}

typedef struct {
    int late_seen;
    unsigned long long late_callbacks;
} result_t;

/* Returns -1 in late_seen when io_uring is unavailable. */
static result_t run_batch(int spin_us_each) {
    result_t res = {0, 0};
    handled = 0;
    late_seen = 0;
    spin_us_per_cb = spin_us_each;
    assert(!cwist_reactor_dispatch_late()); /* not dispatching */
    loop = cwist_reactor_create();
    assert(loop);
    if (loop->impl.use_epoll) {
        cwist_reactor_destroy(loop);
        res.late_seen = -1;
        return res;
    }
    for (int i = 0; i < BATCH; i++) {
        assert(pipe(pipes[i]) == 0);
        char b = 'x';
        assert(write(pipes[i][1], &b, 1) == 1);
    }
    /* Already readable: every POLL_ADD completes before the first wait, so
     * all callbacks land in one round. */
    for (int i = 0; i < BATCH; i++) assert(cwist_reactor_add(loop, pipes[i][0], conn_cb, NULL, 0));
    cwist_reactor_run(loop);
    assert(handled == BATCH);
    assert(!cwist_reactor_dispatch_late());
    res.late_seen = late_seen;
    res.late_callbacks = (unsigned long long)loop->late_callbacks;
    for (int i = 0; i < BATCH; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    cwist_reactor_destroy(loop);
    return res;
}

static result_t run_in_child(const char *late_env, int spin_us_each) {
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        if (late_env)
            setenv("CWIST_REACTOR_LATE_US", late_env, 1);
        else
            unsetenv("CWIST_REACTOR_LATE_US");
        result_t r = run_batch(spin_us_each);
        ssize_t w = write(fds[1], &r, sizeof(r));
        (void)w;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    result_t r = {-2, 0};
    ssize_t n = read(fds[0], &r, sizeof(r));
    close(fds[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(n == (ssize_t)sizeof(r));
    return r;
}

int main(void) {
    /* Default threshold (4 ms): 5 ms callbacks make every callback after the
     * first one late. */
    result_t def = run_in_child(NULL, 5000);
    if (def.late_seen == -1) {
        printf("SKIP: io_uring unavailable on this host\n");
        return 0;
    }
    printf("default: late callbacks %d/%d (counter %llu)\n", def.late_seen, BATCH,
           def.late_callbacks);
    assert(def.late_seen == BATCH - 1);
    assert(def.late_callbacks == (unsigned long long)(BATCH - 1));

    /* Fast callbacks never cross the threshold. */
    result_t fast = run_in_child(NULL, 10);
    printf("fast: late callbacks %d/%d\n", fast.late_seen, BATCH);
    assert(fast.late_seen == 0 && fast.late_callbacks == 0);

    /* 0 disables late mode however long the round runs. */
    result_t off = run_in_child("0", 5000);
    printf("off: late callbacks %d/%d\n", off.late_seen, BATCH);
    assert(off.late_seen == 0 && off.late_callbacks == 0);

    printf("late-round mode (CWIST_REACTOR_LATE_US) test passed\n");
    return 0;
}
