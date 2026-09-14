#define _POSIX_C_SOURCE 200809L
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/sys/job/scheduler.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No assert(): test operations and checks must survive -DNDEBUG. */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

enum { JOBS = 160 };
static bool runtime_only;
static bool full_gc = true;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool release;
    unsigned calls;
    cwist_io_queue *queue;
    cwist_scheduler_t *scheduler;
} fixture;

static void init(fixture *f) {
    memset(f, 0, sizeof(*f));
    CHECK(pthread_mutex_init(&f->lock, NULL) == 0);
    CHECK(pthread_cond_init(&f->cond, NULL) == 0);
}

static void finish(fixture *f) {
    CHECK(pthread_cond_destroy(&f->cond) == 0);
    CHECK(pthread_mutex_destroy(&f->lock) == 0);
}

static void pending(const char *where, size_t expected) {
    size_t actual = cwist_gc_scope_pending_count();
    fprintf(stderr, "scope %s: actual=%zu expected=%zu\n", where, actual, expected);
    if (!runtime_only) CHECK(actual == expected);
}

static void tick(void) {
    /* No sleeps or probabilistic allocator reuse: all donors have joined. */
    for (unsigned i = 0; i < 16; ++i) cwist_gc_pipeline_tick();
}

static void count_job(void *arg) {
    fixture *f = arg; /* Borrowed opaque argument; queue must not free it. */
    CHECK(pthread_mutex_lock(&f->lock) == 0);
    ++f->calls;
    CHECK(pthread_cond_broadcast(&f->cond) == 0);
    CHECK(pthread_mutex_unlock(&f->lock) == 0);
}

static void blocked_job(void *arg) {
    fixture *f = arg;
    CHECK(pthread_mutex_lock(&f->lock) == 0);
    f->entered = true;
    CHECK(pthread_cond_broadcast(&f->cond) == 0);
    while (!f->release) CHECK(pthread_cond_wait(&f->cond, &f->lock) == 0);
    CHECK(pthread_mutex_unlock(&f->lock) == 0);
}

static void wait_blocked(fixture *f) {
    CHECK(pthread_mutex_lock(&f->lock) == 0);
    while (!f->entered) CHECK(pthread_cond_wait(&f->cond, &f->lock) == 0);
    CHECK(pthread_mutex_unlock(&f->lock) == 0);
}

static void release_consumer(fixture *f) {
    CHECK(pthread_mutex_lock(&f->lock) == 0);
    f->release = true;
    CHECK(pthread_cond_broadcast(&f->cond) == 0);
    CHECK(pthread_mutex_unlock(&f->lock) == 0);
}

static void wait_calls(fixture *f, unsigned expected) {
    CHECK(pthread_mutex_lock(&f->lock) == 0);
    while (f->calls < expected) CHECK(pthread_cond_wait(&f->cond, &f->lock) == 0);
    CHECK(f->calls == expected);
    CHECK(pthread_mutex_unlock(&f->lock) == 0);
}

static void *create_queue(void *arg) {
    fixture *f = arg;
    pending("queue creator before", 0);
    f->queue = cwist_io_queue_create(8);
    CHECK(f->queue != NULL);
    pending("queue creator after", 0);
    return NULL; /* Real pthread TLS destructor runs before join completes. */
}

static void *run_queue(void *arg) {
    cwist_io_queue_run(((fixture *)arg)->queue);
    return NULL;
}

static void *create_scheduler(void *arg) {
    fixture *f = arg;
    pending("scheduler creator before", 0);
    f->scheduler = cwist_scheduler_create(1, 8);
    CHECK(f->scheduler != NULL);
    pending("scheduler creator after", 0);
    return NULL;
}

static void join(pthread_t thread) {
    CHECK(pthread_join(thread, NULL) == 0);
}

static void queue_creator(void) {
    fixture f;
    init(&f);
    pthread_t creator;
    CHECK(pthread_create(&creator, NULL, create_queue, &f) == 0);
    join(creator);
    tick();
    CHECK(cwist_io_queue_submit(f.queue, count_job, &f));
    pthread_t consumer;
    CHECK(pthread_create(&consumer, NULL, run_queue, &f) == 0);
    wait_calls(&f, 1);
    /* stop requests termination; do not require a stopped backend to drain. */
    cwist_io_queue_stop(f.queue);
    join(consumer);
    CHECK(f.calls == 1);
    cwist_io_queue_destroy(f.queue);
    tick();
    pending("queue creator receiver after destroy", 0);
    finish(&f);
}

static void scheduler_creator(void) {
    fixture f;
    init(&f);
    pthread_t creator;
    CHECK(pthread_create(&creator, NULL, create_scheduler, &f) == 0);
    join(creator);
    tick();
    CHECK(cwist_scheduler_submit(f.scheduler, count_job, &f));
    wait_calls(&f, 1);
    cwist_scheduler_destroy(f.scheduler);
    tick();
    pending("scheduler creator receiver after destroy", 0);
    finish(&f);
}

static void owned_job(void *arg) {
    fixture *f = *(fixture **)arg;
    count_job(f);
    cwist_free(arg); /* Only the callback owns this explicitly handed-off payload. */
}

static void *submit_donor(void *arg) {
    fixture *f = arg;
    /* A separately owned tracked payload must stay in the donor's scope:
     * queue/scheduler ownership must never traverse opaque job arguments. */
    void *owned = cwist_alloc(32);
    CHECK(owned != NULL);
    pending("donor own allocation", full_gc ? 1 : 0);
    if (f->scheduler) {
        /* Grow the delayed heap beyond its initial 16 slots. It is allocated
         * by cwist_realloc, which is deliberately not scope-tracked. Jobs
         * remain pending until destroy; no test depends on timer sleeps. */
        for (unsigned i = 0; i < 33; ++i)
            CHECK(cwist_scheduler_schedule(f->scheduler, count_job, f, 86400000));
        CHECK(cwist_scheduler_pending_count(f->scheduler) == 33);
    }
    *(fixture **)owned = f;
    for (unsigned i = 0; i < JOBS - 1; ++i) {
        if (f->scheduler) CHECK(cwist_scheduler_submit(f->scheduler, count_job, f));
        else CHECK(cwist_io_queue_submit(f->queue, count_job, f));
    }
    if (f->scheduler) CHECK(cwist_scheduler_submit(f->scheduler, owned_job, owned));
    else CHECK(cwist_io_queue_submit(f->queue, owned_job, owned));
    pending("donor after publication", full_gc ? 1 : 0);
    /* The submit APIs must leave this opaque argument in our scope; only
     * its allocating caller can authorize the payload ownership transfer. */
    if (full_gc) CHECK(cwist_gc_scope_disown(owned));
    pending("donor after payload handoff", 0);
    return NULL;
}

static void donor_exit(bool scheduler) {
    fixture f;
    init(&f);
    pthread_t consumer, donor;
    if (scheduler) {
        f.scheduler = cwist_scheduler_create(1, 8);
        CHECK(f.scheduler != NULL);
        CHECK(cwist_scheduler_submit(f.scheduler, blocked_job, &f));
    } else {
        f.queue = cwist_io_queue_create(8);
        CHECK(f.queue != NULL);
        CHECK(cwist_io_queue_submit(f.queue, blocked_job, &f));
        CHECK(pthread_create(&consumer, NULL, run_queue, &f) == 0);
    }
    wait_blocked(&f); /* Only consumer is inside callback, outside EBR. */
    CHECK(pthread_create(&donor, NULL, submit_donor, &f) == 0);
    join(donor); /* No drain is possible before donor TLS cleanup. */
    tick();
    release_consumer(&f);
    wait_calls(&f, JOBS);
    if (scheduler) {
        CHECK(cwist_scheduler_pending_count(f.scheduler) == 33);
        cwist_scheduler_destroy(f.scheduler);
    } else {
        cwist_io_queue_stop(f.queue);
        join(consumer);
        cwist_io_queue_destroy(f.queue);
    }
    tick();
    pending("donor receiver after destroy", 0);
    finish(&f);
}

int main(int argc, char **argv) {
    CHECK(argc <= 3);
    if (argc > 1) {
        CHECK(!strcmp(argv[1], "gc") || !strcmp(argv[1], "nogc") || !strcmp(argv[1], "runtime"));
        full_gc = strcmp(argv[1], "nogc") != 0;
        runtime_only = !strcmp(argv[1], "runtime");
    }
    alarm(30); /* Bound any synchronization regression, never schedule via sleeps. */
    cwist_full_gc(full_gc);
    CHECK(cwist_full_gc_enabled() == full_gc);
    const char *which = argc > 2 ? argv[2] : "all";
    CHECK(!strcmp(which, "all") || !strcmp(which, "queue-creator") ||
          !strcmp(which, "queue-donor") || !strcmp(which, "scheduler-creator") ||
          !strcmp(which, "scheduler-donor"));
    if (!strcmp(which, "all") || !strcmp(which, "queue-creator")) queue_creator();
    if (!strcmp(which, "all") || !strcmp(which, "queue-donor")) donor_exit(false);
    if (!strcmp(which, "all") || !strcmp(which, "scheduler-creator")) scheduler_creator();
    if (!strcmp(which, "all") || !strcmp(which, "scheduler-donor")) donor_exit(true);
    alarm(0);
    printf("PASS gc job handoff (%s, %s)\n", full_gc ? "gc" : "nogc", which);
    return 0;
}
