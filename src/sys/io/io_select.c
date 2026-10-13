#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/**
 * @file io_select.c
 * @brief Job queue fallback used when no platform-specific async backend is selected.
 *
 * A Michael & Scott two-lock queue: producers append under tail_lock and
 * consumers pop under head_lock, around a dummy sentinel node. The lock-free
 * variant needs safe memory reclamation (see io_queue.c), which this portable
 * fallback does without: a node is freed only by a consumer holding
 * head_lock, after it stopped being the sentinel, and no producer reads a
 * node after linking past it, so nothing can still hold a freed node.
 */

typedef struct job_node {
    cwist_job_func func;
    void *arg;
    _Atomic(struct job_node *) next;
} job_node_t;

struct cwist_io_queue {
    job_node_t *head; /* guarded by head_lock */
    job_node_t *tail; /* guarded by tail_lock */
    pthread_mutex_t head_lock;
    pthread_mutex_t tail_lock;
    atomic_size_t pending_jobs;
    atomic_bool running;
    pthread_mutex_t sleep_lock;
    pthread_cond_t sleep_cond;
};

/**
 * @brief Allocate a queue node for a submitted job.
 * @param func Callback to execute.
 * @param arg User data forwarded to @p func.
 * @return Newly allocated node, or NULL when allocation fails.
 */
static job_node_t *cwist_job_node_create(cwist_job_func func, void *arg) {
    job_node_t *node = cwist_alloc(sizeof(*node));
    if (!node) return NULL;
    node->func = func;
    node->arg = arg;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    return node;
}

/**
 * @brief Release a queue node previously allocated for a job or sentinel.
 * @param node Node to destroy.
 */
static void cwist_job_node_destroy(job_node_t *node) {
    if (!node) return;
    cwist_free(node);
}

/**
 * @brief Append a node to the queue and wake a sleeper when needed.
 * @param q Queue receiving the new job.
 * @param node Node to append.
 */
static void cwist_queue_push(cwist_io_queue *q, job_node_t *node) {
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    pthread_mutex_lock(&q->tail_lock);
    /* next is atomic because a consumer may read the sentinel's next under
     * head_lock while it is also the tail being linked here. */
    atomic_store_explicit(&q->tail->next, node, memory_order_release);
    q->tail = node;
    pthread_mutex_unlock(&q->tail_lock);

    size_t prev = atomic_fetch_add_explicit(&q->pending_jobs, 1, memory_order_release);
    if (prev == 0) {
        pthread_mutex_lock(&q->sleep_lock);
        pthread_cond_signal(&q->sleep_cond);
        pthread_mutex_unlock(&q->sleep_lock);
    }
}

/**
 * @brief Pop the next job from the queue.
 * The popped node becomes the new sentinel, which the next pop frees once
 * this one releases head_lock, so the callback and argument are copied out
 * under the lock and the node is not touched again.
 * @param q Queue to consume from.
 * @param func Receives the job callback.
 * @param arg Receives the job argument.
 * @return true when a job was popped, false when the queue is empty.
 */
static bool cwist_queue_pop(cwist_io_queue *q, cwist_job_func *func, void **arg) {
    pthread_mutex_lock(&q->head_lock);
    job_node_t *head = q->head;
    job_node_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
    if (!next) {
        pthread_mutex_unlock(&q->head_lock);
        return false;
    }
    *func = next->func;
    *arg = next->arg;
    q->head = next;
    pthread_mutex_unlock(&q->head_lock);
    /* head was the sentinel and is no longer the tail (it has a next), so
     * no producer or consumer can reach it any more. */
    cwist_job_node_destroy(head);
    return true;
}

/**
 * @brief Sleep until work arrives or shutdown is requested.
 * @param q Queue whose condition variable should be observed.
 * @return true when work may still arrive, false when the queue is stopping and empty.
 */
static bool cwist_queue_wait(cwist_io_queue *q) {
    pthread_mutex_lock(&q->sleep_lock);
    while (atomic_load_explicit(&q->pending_jobs, memory_order_acquire) == 0 &&
           atomic_load_explicit(&q->running, memory_order_acquire)) {
        pthread_cond_wait(&q->sleep_cond, &q->sleep_lock);
    }
    bool should_continue = atomic_load_explicit(&q->running, memory_order_acquire) ||
                           atomic_load_explicit(&q->pending_jobs, memory_order_acquire) > 0;
    pthread_mutex_unlock(&q->sleep_lock);
    return should_continue;
}

/**
 * @brief Allocate a queue instance and install its sentinel node.
 * @param capacity Requested capacity hint. Currently unused by the select fallback.
 * @return Queue handle, or NULL when allocation or synchronization setup fails.
 */
cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    (void)capacity;
    cwist_io_queue *q = cwist_alloc(sizeof(*q));
    if (!q) return NULL;

    job_node_t *stub = cwist_job_node_create(NULL, NULL);
    if (!stub) {
        cwist_free(q);
        return NULL;
    }

    q->head = stub;
    q->tail = stub;
    atomic_init(&q->pending_jobs, 0);
    atomic_store_explicit(&q->running, true, memory_order_release);

    if (pthread_mutex_init(&q->head_lock, NULL) != 0) goto fail_head;
    if (pthread_mutex_init(&q->tail_lock, NULL) != 0) goto fail_tail;
    if (pthread_mutex_init(&q->sleep_lock, NULL) != 0) goto fail_sleep;
    if (pthread_cond_init(&q->sleep_cond, NULL) != 0) goto fail_cond;
    return q;

fail_cond:
    pthread_mutex_destroy(&q->sleep_lock);
fail_sleep:
    pthread_mutex_destroy(&q->tail_lock);
fail_tail:
    pthread_mutex_destroy(&q->head_lock);
fail_head:
    cwist_job_node_destroy(stub);
    cwist_free(q);
    return NULL;
}

/**
 * @brief Submit a callback for asynchronous execution by the queue runner.
 * @param q Queue that should own the job.
 * @param func Callback to execute.
 * @param arg User payload forwarded to @p func.
 * @return true on success, or false when the queue is shutting down or OOM.
 */
bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    if (!q || !func) return false;
    if (!atomic_load_explicit(&q->running, memory_order_acquire)) {
        return false;
    }

    job_node_t *node = cwist_job_node_create(func, arg);
    if (!node) return false;
    cwist_queue_push(q, node);
    return true;
}

/**
 * @brief Drain queued jobs until shutdown is requested and no work remains.
 * @param q Queue to run on the current thread.
 */
void cwist_io_queue_run(cwist_io_queue *q) {
    if (!q) return;
    while (atomic_load_explicit(&q->running, memory_order_acquire) ||
           atomic_load_explicit(&q->pending_jobs, memory_order_acquire) > 0) {
        cwist_job_func func = NULL;
        void *arg = NULL;
        if (!cwist_queue_pop(q, &func, &arg)) {
            if (!cwist_queue_wait(q)) {
                break;
            }
            continue;
        }

        atomic_fetch_sub_explicit(&q->pending_jobs, 1, memory_order_acq_rel);
        if (func) func(arg);
    }
}

/**
 * @brief Request that runners stop without freeing the queue.
 * @param q Queue instance to stop.
 */
void cwist_io_queue_stop(cwist_io_queue *q) {
    if (!q) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
    pthread_mutex_lock(&q->sleep_lock);
    pthread_cond_broadcast(&q->sleep_cond);
    pthread_mutex_unlock(&q->sleep_lock);
}

/**
 * @brief Stop the queue, wake sleepers, and free every remaining node.
 * @param q Queue instance to destroy.
 */
void cwist_io_queue_destroy(cwist_io_queue *q) {
    if (!q) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
    pthread_mutex_lock(&q->sleep_lock);
    pthread_cond_broadcast(&q->sleep_cond);
    pthread_mutex_unlock(&q->sleep_lock);

    /* Runners are gone by contract; the locks only pair with the last pops. */
    pthread_mutex_lock(&q->head_lock);
    job_node_t *node = q->head;
    pthread_mutex_unlock(&q->head_lock);
    while (node) {
        job_node_t *next = atomic_load_explicit(&node->next, memory_order_acquire);
        cwist_job_node_destroy(node);
        node = next;
    }

    pthread_cond_destroy(&q->sleep_cond);
    pthread_mutex_destroy(&q->sleep_lock);
    pthread_mutex_destroy(&q->tail_lock);
    pthread_mutex_destroy(&q->head_lock);
    cwist_free(q);
}
