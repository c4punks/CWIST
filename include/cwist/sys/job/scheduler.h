/**
 * @file scheduler.h
 * @brief Background job scheduler / delayed task queue for CWIST.
 */

#ifndef __CWIST_SCHEDULER_H__
#define __CWIST_SCHEDULER_H__

#include <cwist/sys/io/cwist_io.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque background job scheduler handle.
 */
typedef struct cwist_scheduler cwist_scheduler_t;

/**
 * @brief Create a scheduler with a pool of background workers.
 *
 * @param worker_count   Number of worker threads. Must be >= 1.
 * @param queue_capacity Capacity hint forwarded to the underlying io queue.
 * @return Scheduler handle, or NULL on failure.
 */
cwist_scheduler_t *cwist_scheduler_create(size_t worker_count, size_t queue_capacity);

/**
 * @brief Destroy the scheduler and stop all worker threads.
 *
 * Pending immediate jobs are drained before shutdown. Delayed jobs that have
 * not yet fired are dropped.
 */
void cwist_scheduler_destroy(cwist_scheduler_t *s);

/**
 * @brief Submit a job to be executed as soon as a worker is available.
 * @return true on success, false when the scheduler is stopping or OOM.
 */
bool cwist_scheduler_submit(cwist_scheduler_t *s, cwist_job_func func, void *arg);

/**
 * @brief Schedule a job to run after a relative delay.
 *
 * @param delay_ms Milliseconds to wait before executing the job.
 * @return true on success, false on failure.
 */
bool cwist_scheduler_schedule(cwist_scheduler_t *s, cwist_job_func func, void *arg,
                              uint64_t delay_ms);

/**
 * @brief Number of delayed jobs currently waiting to fire.
 */
size_t cwist_scheduler_pending_count(cwist_scheduler_t *s);

/**
 * @brief Callback invoked for each pending job that is cancelled.
 *
 * @param arg The argument that would have been passed to the job function.
 */
typedef void (*cwist_scheduler_drop_cb)(void *arg);

/**
 * @brief Cancel all delayed jobs that have not yet fired.
 *
 * Dequeues every job still waiting on the delayed-job heap. For each job,
 * @p drop_cb is called with the job's arg so the caller can reclaim any
 * heap-allocated state. If @p drop_cb is NULL the args are discarded.
 *
 * Jobs that have already been submitted to the io queue by the timer thread
 * will still run; this function only removes jobs still on the heap.
 *
 * Safe to call concurrently with cwist_scheduler_schedule(): both operations
 * hold the same mutex.
 *
 * @param s       Scheduler whose pending jobs are to be cancelled.
 * @param drop_cb Optional destructor called once per cancelled job's arg.
 * @return        Number of jobs cancelled.
 */
size_t cwist_scheduler_cancel_pending(cwist_scheduler_t *s,
                                      cwist_scheduler_drop_cb drop_cb);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SCHEDULER_H__ */
