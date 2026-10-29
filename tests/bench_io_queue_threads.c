#define _POSIX_C_SOURCE 200809L
/* Measures short-lived queue workers. libttak v3.5.2 can reuse epoch thread
 * state only when a runner deregisters before exiting. */
#include <cwist/sys/io/cwist_io.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void noop_job(void *arg) {
    (void)arg;
}

static void *run_queue(void *arg) {
    cwist_io_queue_run(arg);
    return NULL;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static void run_iterations(size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        cwist_io_queue *queue = cwist_io_queue_create(0);
        assert(queue);
        assert(cwist_io_queue_submit(queue, noop_job, NULL));
        cwist_io_queue_stop(queue);

        pthread_t thread;
        assert(pthread_create(&thread, NULL, run_queue, queue) == 0);
        pthread_join(thread, NULL);
        cwist_io_queue_destroy(queue);
    }
}

int main(int argc, char **argv) {
    size_t iterations = argc > 1 ? strtoull(argv[1], NULL, 10) : 4000;
    run_iterations(32);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    run_iterations(iterations);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = elapsed_ms(start, end);
    printf("iterations=%zu elapsed_ms=%.3f thread_jobs_per_sec=%.0f\n", iterations, ms,
           iterations * 1000.0 / ms);
    return 0;
}
