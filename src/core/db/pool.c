/**
 * @file pool.c
 * @brief SQLite connection pool implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pool.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cwist_db_pool {
    char *path;
    size_t max_conns;
    cwist_db **conns;
    bool *available;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
};

static atomic_ulong pool_sequence = 1;

/** @brief Build the error reported when a pooled operation cannot acquire a connection.
 * @return Error tagged @c CWIST_ERR_INT16 with code -1. The caller owns the result. */
static cwist_error_t pool_error(void) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

/** @brief Pick the clock used for pool condition-variable deadlines.
 * @return @c CLOCK_REALTIME on BSD/macOS (no monotonic condvar support), otherwise @c CLOCK_MONOTONIC. */
static clockid_t pool_cond_clock(void) {
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    return CLOCK_REALTIME;
#else
    return CLOCK_MONOTONIC;
#endif
}

/** @brief Initialize a condition variable with the pool's deadline clock.
 * @param cond Condition variable to initialize; must not be NULL.
 * @retval true Initialization succeeded.
 * @retval false Initialization failed; @a cond is left in an unusable state. */
static bool pool_cond_init(pthread_cond_t *cond) {
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    return pthread_cond_init(cond, NULL) == 0;
#else
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return false;
    bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
              pthread_cond_init(cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    return ok;
#endif
}

/** @brief Compute an absolute deadline @a timeout_ms milliseconds from now.
 * @param deadline Output absolute deadline on the pool condvar clock; must not be NULL.
 * @param timeout_ms Timeout in milliseconds. */
static void pool_deadline_after_ms(struct timespec *deadline, int timeout_ms) {
    clock_gettime(pool_cond_clock(), deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

/** @brief Derive the SQLite open path for a pooled database.
 * @param path Requested database path.
 * @return Newly allocated open path (caller frees), or NULL on allocation failure. The
 *         ":memory:" pseudo-path is rewritten to a unique shared-cache URI so every pooled
 *         connection sees one in-memory database. */
static char *pool_open_path(const char *path) {
    if (strcmp(path, ":memory:") != 0) return cwist_strdup(path);
    unsigned long seq = atomic_fetch_add_explicit(&pool_sequence, 1, memory_order_relaxed);
    char buffer[96];
    int written = snprintf(buffer, sizeof(buffer), "file:cwist-pool-%lu?mode=memory&cache=shared", seq);
    return written > 0 && (size_t)written < sizeof(buffer) ? cwist_strdup(buffer) : NULL;
}

/** @brief Create a pool of @a max_conns SQLite connections to @a path.
 * @param path Database path, or ":memory:" for a shared in-memory database.
 * @param max_conns Maximum number of pooled connections; must be > 0.
 * @return New pool on success, or NULL on invalid arguments or any allocation/open failure.
 *         On success all connections start idle. Thread-safe. */
cwist_db_pool_t *cwist_db_pool_create(const char *path, size_t max_conns) {
    if (!path || max_conns == 0) return NULL;

    cwist_db_pool_t *pool = cwist_alloc(sizeof(*pool));
    if (!pool) return NULL;
    pool->path = cwist_strdup(path);
    pool->open_path = pool_open_path(path);
    pool->conns = cwist_alloc_array(max_conns, sizeof(*pool->conns));
    pool->idle_slots = cwist_alloc_array(max_conns, sizeof(*pool->idle_slots));
    pool->leased = cwist_alloc_array(max_conns, sizeof(*pool->leased));
    bool mutex_ready = false;
    bool cond_ready = false;
    if (!pool->path || !pool->open_path || !pool->conns || !pool->idle_slots || !pool->leased) goto fail;
    if (pthread_mutex_init(&pool->mtx, NULL) != 0) goto fail;
    mutex_ready = true;
    if (!pool_cond_init(&pool->cond)) goto fail;
    cond_ready = true;
    pool->max_conns = max_conns;
    pool->conns = cwist_alloc_array(max_conns, sizeof(cwist_db *));
    pool->available = cwist_alloc_array(max_conns, sizeof(bool));
    if (!pool->path || !pool->conns || !pool->available) goto fail;

    for (size_t i = 0; i < max_conns; i++) {
        cwist_error_t dberr = cwist_db_open(&pool->conns[i], path);
        if (dberr.error.err_i16 != 0) goto fail;
        pool->available[i] = true;
    }

    pthread_mutex_init(&pool->mtx, NULL);
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    pthread_cond_init(&pool->cond, NULL);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
              pthread_cond_init(&pool->cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    if (!ok) {
        pthread_cond_init(&pool->cond, NULL);
    }
#endif
    return pool;

fail:
    if (pool->conns) for (size_t i = 0; i < max_conns; ++i) cwist_db_close(pool->conns[i]);
    if (cond_ready) pthread_cond_destroy(&pool->cond);
    if (mutex_ready) pthread_mutex_destroy(&pool->mtx);
    cwist_free(pool->leased); cwist_free(pool->idle_slots); cwist_free(pool->conns); cwist_free(pool->open_path); cwist_free(pool->path); cwist_free(pool);
    return NULL;
}

/** @brief Take an idle connection from the pool; caller must hold @c pool->mtx.
 * @param pool Pool to lease from.
 * @return Leased connection, or NULL if the pool is closing or has no idle connections. */
static cwist_db *pool_acquire_locked(cwist_db_pool_t *pool) {
    if (pool->closing || pool->idle_count == 0) return NULL;
    size_t slot = pool->idle_slots[--pool->idle_count];
    pool->leased[slot] = true;
    ++pool->in_use;
    return pool->conns[slot];
}

/** @brief Lease a pooled connection, blocking indefinitely until one is available.
 * @param pool Pool to lease from; must not be NULL.
 * @return Leased connection to pass to cwist_db_pool_release(), or NULL if the pool is NULL or closing.
 *         Thread-safe; the caller must not use the connection after releasing it. */
cwist_db *cwist_db_pool_acquire(cwist_db_pool_t *pool) {
    if (!pool) return NULL;
    pthread_mutex_lock(&pool->mtx);
    while (!pool->closing && pool->idle_count == 0) pthread_cond_wait(&pool->cond, &pool->mtx);
    cwist_db *db = pool_acquire_locked(pool);
    pthread_mutex_unlock(&pool->mtx);
    return db;
}

/** @brief Lease a pooled connection, waiting at most @a timeout_ms.
 * @param pool Pool to lease from; must not be NULL.
 * @param timeout_ms Maximum wait in milliseconds; must be >= 0.
 * @return Leased connection to pass to cwist_db_pool_release(), or NULL on invalid arguments,
 *         on timeout, or if the pool is closing. Thread-safe. */
cwist_db *cwist_db_pool_acquire_timeout(cwist_db_pool_t *pool, int timeout_ms) {
    if (!pool || timeout_ms < 0) return NULL;
    struct timespec deadline;
    pool_deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&pool->mtx);
    while (!pool->closing && pool->idle_count == 0) {
        if (pthread_cond_timedwait(&pool->cond, &pool->mtx, &deadline) != 0) break;
    }
    cwist_db *db = pool_acquire_locked(pool);
    pthread_mutex_unlock(&pool->mtx);
    return db;
}

/** @brief Return a leased connection to the pool and wake one waiting acquirer.
 * @param pool Pool that owns the connection.
 * @param conn Connection previously returned by an acquire call; must belong to @a pool
 *             and still be leased. Invalid or already-released connections are ignored.
 * Thread-safe. */
void cwist_db_pool_release(cwist_db_pool_t *pool, cwist_db *conn) {
    if (!pool || !conn) return;
    pthread_mutex_lock(&pool->mtx);
    size_t slot = conn->pool_slot;
    if (slot < pool->max_conns && pool->conns[slot] == conn && pool->leased[slot]) {
        pool->leased[slot] = false;
        pool->idle_slots[pool->idle_count++] = slot;
        --pool->in_use;
        pthread_cond_signal(&pool->cond);
    }
    pthread_mutex_unlock(&pool->mtx);
}

/** @brief Count connections currently leased from the pool.
 * @param pool Pool to inspect.
 * @return Number of connections in use, or 0 if @a pool is NULL. Thread-safe. */
size_t cwist_db_pool_in_use(cwist_db_pool_t *pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mtx); size_t count = pool->in_use; pthread_mutex_unlock(&pool->mtx);
    return count;
}

/** @brief Close a pool, waiting up to @a timeout_ms for leased connections.
 * @param pool Pool to destroy; must not be NULL.
 * @param timeout_ms Wait limit in milliseconds, or -1 to wait indefinitely for all
 *                   connections to be released.
 * @retval true All connections were released and the pool (and every connection) was closed and freed.
 * @retval false The wait timed out with connections still leased; the pool is left alive and closing,
 *               so further acquires fail and the caller must retry the destroy.
 * Thread-safe. */
bool cwist_db_pool_destroy_timeout(cwist_db_pool_t *pool, int timeout_ms) {
    if (!pool || timeout_ms < -1) return false;
    pthread_mutex_lock(&pool->mtx);
    pool->closing = true;
    pthread_cond_broadcast(&pool->cond);
    if (timeout_ms < 0) {
        while (pool->in_use != 0) pthread_cond_wait(&pool->cond, &pool->mtx);
    } else {
        struct timespec deadline;
        pool_deadline_after_ms(&deadline, timeout_ms);
        while (pool->in_use != 0) {
            int rc = pthread_cond_timedwait(&pool->cond, &pool->mtx, &deadline);
            if (rc != 0 && pool->in_use != 0) {
                pthread_mutex_unlock(&pool->mtx);
                return false;
            }
        }
    }
    pthread_mutex_unlock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; ++i) cwist_db_close(pool->conns[i]);
    pthread_cond_destroy(&pool->cond); pthread_mutex_destroy(&pool->mtx);
    cwist_free(pool->leased); cwist_free(pool->idle_slots); cwist_free(pool->conns); cwist_free(pool->open_path); cwist_free(pool->path); cwist_free(pool);
    return true;
}

/** @brief Close a pool, blocking until all leased connections are released.
 * @param pool Pool to destroy; no-op if NULL. Equivalent to
 *             cwist_db_pool_destroy_timeout(pool, -1). */
void cwist_db_pool_destroy(cwist_db_pool_t *pool) {
    (void)cwist_db_pool_destroy_timeout(pool, -1);
}

/** @brief Execute a SQL statement on a leased pooled connection.
 * @param pool Pool to run the statement on.
 * @param sql SQL statement to execute.
 * @return Execution result, or an INT16 error with code -1 if no connection could be acquired.
 *         The caller owns the result. */
cwist_error_t cwist_db_pool_exec(cwist_db_pool_t *pool, const char *sql) {
    cwist_db *db = cwist_db_pool_acquire(pool);
    if (!db) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_db_exec(db, sql);
    cwist_db_pool_release(pool, db);
    return err;
}

/** @brief Run a query on a leased pooled connection.
 * @param pool Pool to run the query on.
 * @param sql SQL query to execute.
 * @param result Output JSON result written by the query; caller owns and must free it.
 * @return Query result, or an INT16 error with code -1 if no connection could be acquired.
 *         The connection is released back to the pool before returning. */
cwist_error_t cwist_db_pool_query(cwist_db_pool_t *pool, const char *sql, cJSON **result) {
    cwist_db *db = cwist_db_pool_acquire(pool);
    if (!db) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_db_query(db, sql, result);
    cwist_db_pool_release(pool, db);
    return err;
}
