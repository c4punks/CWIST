#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pool.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

enum { POOL_TEST_WAITERS = 8 };

typedef struct {
    cwist_db_pool_t *pool;
    atomic_int started;
} pool_waiter_ctx;

static void *blocked_acquire(void *arg) {
    pool_waiter_ctx *ctx = arg;
    atomic_fetch_add_explicit(&ctx->started, 1, memory_order_release);
    return cwist_db_pool_acquire(ctx->pool);
}

static void test_destroy_with_waiters(void) {
    cwist_db_pool_t *pool = cwist_db_pool_create(":memory:", 1);
    assert(pool != NULL);
    cwist_db *held = cwist_db_pool_acquire(pool);
    assert(held != NULL);

    pool_waiter_ctx ctx = {.pool = pool};
    atomic_init(&ctx.started, 0);
    pthread_t threads[POOL_TEST_WAITERS];
    for (int i = 0; i < POOL_TEST_WAITERS; i++)
        assert(pthread_create(&threads[i], NULL, blocked_acquire, &ctx) == 0);
    while (atomic_load_explicit(&ctx.started, memory_order_acquire) != POOL_TEST_WAITERS) {
        struct timespec yield = {.tv_nsec = 1000000L};
        nanosleep(&yield, NULL);
    }
    struct timespec pause = {.tv_nsec = 50000000L};
    nanosleep(&pause, NULL);

    cwist_db_pool_release(pool, held);
    assert(cwist_db_pool_destroy_timeout(pool, 1000));
    for (int i = 0; i < POOL_TEST_WAITERS; i++) {
        void *result = (void *)1;
        assert(pthread_join(threads[i], &result) == 0);
        assert(result == NULL);
    }
}

int main(void) {
    /* An unopenable path must fail cleanly instead of crashing. */
    assert(cwist_db_pool_create("/cwist-no-such-dir/pool.db", 4) == NULL);

    cwist_db_pool_t *pool = cwist_db_pool_create(":memory:", 3);
    assert(pool != NULL);

    cwist_error_t err =
        cwist_db_pool_exec(pool, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    assert(cwist_error_is_ok(&err));

    err = cwist_db_pool_exec(pool, "INSERT INTO users (name) VALUES ('alice'), ('bob');");
    assert(cwist_error_is_ok(&err));

    cJSON *result = NULL;
    err = cwist_db_pool_query(pool, "SELECT * FROM users ORDER BY id;", &result);
    assert(cwist_error_is_ok(&err));
    assert(result != NULL);
    assert(cJSON_IsArray(result));
    assert(cJSON_GetArraySize(result) == 2);

    cJSON *first = cJSON_GetArrayItem(result, 0);
    cJSON *name = cJSON_GetObjectItem(first, "name");
    assert(name != NULL && cJSON_IsString(name));
    assert(strcmp(name->valuestring, "alice") == 0);

    cJSON_Delete(result);

    /* Manual acquire/release smoke test. */
    cwist_db *conn = cwist_db_pool_acquire(pool);
    assert(conn != NULL);
    cwist_db_pool_release(pool, conn);

    /* A stale release must be a no-op. */
    cwist_db_pool_release(pool, conn);
    assert(cwist_db_pool_in_use(pool) == 0);

    /* Every connection shares the :memory: database and timeout is bounded. */
    conn = cwist_db_pool_acquire(pool);
    assert(conn != NULL);
    assert(cwist_db_pool_in_use(pool) == 1);
    cwist_db_pool_release(pool, conn);

    cwist_db *one = cwist_db_pool_acquire(pool);
    cwist_db *two = cwist_db_pool_acquire(pool);
    cwist_db *three = cwist_db_pool_acquire(pool);
    assert(one && two && three);
    assert(cwist_db_pool_in_use(pool) == 3);
    assert(cwist_db_pool_acquire_timeout(pool, 5) == NULL);
    cwist_db_pool_release(pool, one);
    cwist_db_pool_release(pool, two);
    cwist_db_pool_release(pool, three);

    /* An orphaned lease must not make shutdown wait forever.  Timed shutdown
     * closes the pool to new borrowers, preserves the leased handle, and can
     * complete safely after its owner returns it. */
    conn = cwist_db_pool_acquire(pool);
    assert(conn != NULL);
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    assert(!cwist_db_pool_destroy_timeout(pool, 5));
    clock_gettime(CLOCK_MONOTONIC, &finished);
    long elapsed_ms = (finished.tv_sec - started.tv_sec) * 1000L +
                      (finished.tv_nsec - started.tv_nsec) / 1000000L;
    assert(elapsed_ms < 500);
    assert(cwist_db_pool_acquire_timeout(pool, 1) == NULL);
    cwist_db_pool_release(pool, conn);
    assert(cwist_db_pool_destroy_timeout(pool, 100));
    test_destroy_with_waiters();
    printf("All DB pool tests passed.\n");
    return 0;
}
