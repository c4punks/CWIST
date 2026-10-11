#define _POSIX_C_SOURCE 200809L
/* Covers full-GC's connection registry (docs/GC.md sections 2-3): a
 * handle explicitly untrack()'d before its normal close is never
 * double-closed by a sweep; a handle left tracked when its owning thread
 * exits is closed exactly once by that thread's TLS destructor; and a
 * handle left tracked when the *process* exits (other threads still
 * "running" in the sense their TLS destructors never get a chance to
 * fire) is still closed by the atexit sweep -- verified via a real
 * subprocess so atexit() actually runs.
 *
 * The #345 cases: a handle tracked on one thread and closed on another is
 * never swept again; a handle adopted by another thread survives the
 * tracking thread's exit and is closed by the adopter's; a close_fn may
 * call back into the registry; a forked child does not close the
 * parent's connections; and untrack stays cheap with many handles
 * tracked across many threads.
 */
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_close_count = 0;

static void close_cb(void *handle) {
    atomic_fetch_add(&g_close_count, 1);
    free(handle);
}

/* --- explicit untrack must prevent the sweep from touching it --- */
static void test_untrack_prevents_sweep(void) {
    cwist_full_gc(true);
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    assert(cwist_conn_registry_untrack(h) == true);
    /* Second untrack: not found (already removed). */
    assert(cwist_conn_registry_untrack(h) == false);
    cwist_conn_registry_flush(); /* would double-close h if untrack didn't work */
    free(h);
    assert(atomic_load(&g_close_count) == 0);
    printf("test_conn_registry: untrack ok\n");
}

/* --- a forgotten handle is closed once when its owning thread exits --- */
static void *thread_forgets_one(void *arg) {
    (void)arg;
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    /* no untrack -- forgotten on purpose */
    return NULL;
}

static void test_thread_exit_sweep(void) {
    int before = atomic_load(&g_close_count);
    pthread_t t;
    assert(pthread_create(&t, NULL, thread_forgets_one, NULL) == 0);
    assert(pthread_join(t, NULL) == 0);
    int after = atomic_load(&g_close_count);
    assert(after - before == 1);
    printf("test_conn_registry: thread-exit sweep ok\n");
}

/* --- process-exit sweep: run in a child process so atexit() fires for
 * real, with another thread still "alive" (never joined) holding a
 * forgotten handle -- its own TLS destructor never runs in that case,
 * only the atexit path can close it. */
static void *thread_forgets_and_blocks(void *arg) {
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    pause(); /* never returns; this thread is still "running" at exit() */
    (void)arg;
    return NULL;
}

/* Registered first (before the tracking thread even starts), so LIFO
 * atexit ordering runs it *last* -- after the connection registry's own
 * atexit sweep has already run and updated g_close_count. */
static void print_close_count_atexit(void) {
    printf("atexit-swept:%d\n", atomic_load(&g_close_count));
    fflush(stdout);
}

static int child_main(void) {
    atexit(print_close_count_atexit);
    cwist_full_gc(true);
    pthread_t t;
    if (pthread_create(&t, NULL, thread_forgets_and_blocks, NULL) != 0) return 2;
    struct timespec ts = {0, 50 * 1000 * 1000}; /* let it track before we exit */
    nanosleep(&ts, NULL);
    exit(0); /* runs atexit handlers; thread_forgets_and_blocks() never returns */
}

static void test_process_exit_sweep(void) {
    /* Use a pipe so the child can report whether close_cb() ran, since
     * process-local state doesn't survive fork(). */
    int fd[2];
    assert(pipe(fd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        child_main();
        _exit(3); /* unreachable: child_main() calls exit() */
    }
    close(fd[1]);
    char buf[256] = {0};
    ssize_t n = read(fd[0], buf, sizeof(buf) - 1);
    close(fd[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(n > 0 && strstr(buf, "atexit-swept:1") != NULL);
    printf("test_conn_registry: process-exit sweep ok\n");
}

/* --- #345: cross-thread close and ownership transfer ------------------- */

typedef struct {
    void *handle;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int stage; /* 0 start, 1 tracked, 2 may exit */
} handoff_t;

static void handoff_wait(handoff_t *h, int stage) {
    pthread_mutex_lock(&h->mu);
    while (h->stage < stage) pthread_cond_wait(&h->cv, &h->mu);
    pthread_mutex_unlock(&h->mu);
}

static void handoff_set(handoff_t *h, int stage) {
    pthread_mutex_lock(&h->mu);
    h->stage = stage;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

/* Tracks a handle, then waits until the main thread says it may exit. */
static void *tracker_thread(void *arg) {
    handoff_t *h = arg;
    h->handle = malloc(1);
    cwist_conn_registry_track(h->handle, close_cb);
    assert(cwist_conn_registry_pending_count() == 1);
    handoff_set(h, 1);
    handoff_wait(h, 2);
    return NULL; /* exit sweep */
}

static void test_cross_thread_untrack(void) {
    int before = atomic_load(&g_close_count);
    handoff_t h = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pthread_t t;
    assert(pthread_create(&t, NULL, tracker_thread, &h) == 0);
    handoff_wait(&h, 1);
    /* Closed explicitly here, on a thread that did not track it. */
    assert(cwist_conn_registry_untrack(h.handle) == true);
    free(h.handle);
    handoff_set(&h, 2);
    assert(pthread_join(t, NULL) == 0);
    /* The tracker's exit sweep must not touch the freed handle (#344). */
    assert(atomic_load(&g_close_count) == before);
    printf("test_conn_registry: cross-thread untrack ok\n");
}

static void *adopter_thread(void *arg) {
    handoff_t *h = arg;
    assert(cwist_conn_registry_adopt(h->handle) == true);
    assert(cwist_conn_registry_pending_count() == 1);
    handoff_set(h, 3); /* adopted */
    handoff_wait(h, 4);
    return NULL; /* exit sweep: closes the adopted handle */
}

static void test_adopt_moves_ownership(void) {
    int before = atomic_load(&g_close_count);
    handoff_t h = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pthread_t tracker, adopter;
    assert(pthread_create(&tracker, NULL, tracker_thread, &h) == 0);
    handoff_wait(&h, 1);
    assert(pthread_create(&adopter, NULL, adopter_thread, &h) == 0);
    handoff_wait(&h, 3);

    /* The tracking thread exits while another thread serves the handle:
     * it used to close it out from under the adopter. */
    handoff_set(&h, 2); /* lets the tracker exit; the adopter waits for 4 */
    assert(pthread_join(tracker, NULL) == 0);
    assert(atomic_load(&g_close_count) == before);

    handoff_set(&h, 4);
    assert(pthread_join(adopter, NULL) == 0);
    assert(atomic_load(&g_close_count) == before + 1);

    void *untracked = malloc(1);
    assert(cwist_conn_registry_adopt(untracked) == false);
    assert(cwist_conn_registry_adopt(NULL) == false);
    free(untracked);
    printf("test_conn_registry: adopt ok\n");
}

/* --- close_fn may call back into the registry --------------------------- */

static void *g_inner;

static void reentrant_close_cb(void *handle) {
    /* Used to deadlock: close_fn ran under the registry's own lock. */
    assert(cwist_conn_registry_untrack(g_inner) == true);
    free(g_inner);
    void *again = malloc(1);
    cwist_conn_registry_track(again, close_cb);
    assert(cwist_conn_registry_untrack(again) == true);
    free(again);
    free(handle);
}

static void *outer_tracker_thread(void *arg) {
    cwist_conn_registry_track(arg, reentrant_close_cb);
    return NULL; /* exit sweep runs reentrant_close_cb */
}

static void test_reentrant_close_fn(void) {
    int before = atomic_load(&g_close_count);
    /* The inner handle is owned by this thread, the outer one by a helper
     * whose exit sweep closes it; that close_fn untracks the inner handle
     * and tracks/untracks a fresh one. */
    g_inner = malloc(1);
    cwist_conn_registry_track(g_inner, close_cb);
    pthread_t t;
    assert(pthread_create(&t, NULL, outer_tracker_thread, malloc(1)) == 0);
    assert(pthread_join(t, NULL) == 0);
    assert(cwist_conn_registry_pending_count() == 0);
    assert(atomic_load(&g_close_count) == before);
    printf("test_conn_registry: re-entrant close_fn ok\n");
}

/* --- fork: the child must not close the parent's connections ------------ */

static void test_fork_child_does_not_sweep(void) {
    int before = atomic_load(&g_close_count);
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    int fd[2];
    assert(pipe(fd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fd[0]);
        int pending = (int)cwist_conn_registry_pending_count();
        cwist_conn_registry_sweep_all(); /* what exit() would run */
        char msg[64];
        int n = snprintf(msg, sizeof(msg), "pending:%d closed:%d", pending,
                         atomic_load(&g_close_count) - before);
        if (write(fd[1], msg, (size_t)n) != n) _exit(4);
        _exit(0);
    }
    close(fd[1]);
    char buf[64] = {0};
    ssize_t n = read(fd[0], buf, sizeof(buf) - 1);
    close(fd[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(n > 0 && strcmp(buf, "pending:0 closed:0") == 0);
    /* The parent still owns it. */
    assert(cwist_conn_registry_pending_count() == 1);
    assert(cwist_conn_registry_untrack(h) == true);
    free(h);
    printf("test_conn_registry: fork child ok\n");
}

/* --- untrack stays cheap with many handles across many threads ---------- */

#define SCALE_THREADS 16
#define SCALE_PER_THREAD 4000

static void *scale_thread(void *arg) {
    void **hs = arg;
    for (int i = 0; i < SCALE_PER_THREAD; i++) {
        hs[i] = malloc(1);
        cwist_conn_registry_track(hs[i], close_cb);
    }
    for (int i = 0; i < SCALE_PER_THREAD; i++) {
        assert(cwist_conn_registry_untrack(hs[i]) == true);
        free(hs[i]);
    }
    return NULL;
}

static void test_untrack_scale(void) {
    int before = atomic_load(&g_close_count);
    static void *hs[SCALE_THREADS][SCALE_PER_THREAD];
    pthread_t t[SCALE_THREADS];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < SCALE_THREADS; i++) assert(pthread_create(&t[i], NULL, scale_thread, hs[i]) == 0);
    for (int i = 0; i < SCALE_THREADS; i++) assert(pthread_join(t[i], NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    assert(atomic_load(&g_close_count) == before);
    printf("test_conn_registry: %d track+untrack in %.1f ms ok\n", SCALE_THREADS * SCALE_PER_THREAD, ms);
}

int main(void) {
    test_untrack_prevents_sweep();
    test_thread_exit_sweep();
    test_process_exit_sweep();
    test_cross_thread_untrack();
    test_adopt_moves_ownership();
    test_reentrant_close_fn();
    test_fork_child_does_not_sweep();
    test_untrack_scale();
    printf("test_conn_registry: ok\n");
    return 0;
}
