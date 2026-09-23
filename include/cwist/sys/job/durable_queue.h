/**
 * @file durable_queue.h
 * @brief Durable (persistent) job queue backends over Redis and NATS.
 *
 * EXPERIMENTAL (v3.7 Phase 4): opt-in tier on top of the existing Redis and
 * NATS clients, separate from the in-process scheduler queue
 * (cwist/sys/job/scheduler.h). Jobs survive process restarts; delivery is
 * at-least-once.
 *
 * Backends:
 * - Redis: stream per queue with a consumer group. Claiming uses
 *   XREADGROUP; unacknowledged jobs are reclaimed via XAUTOCLAIM after the
 *   visibility timeout; nacks and exhausted retries are dead-lettered to a
 *   per-queue list (`<prefix>:dead`).
 * - NATS: JetStream stream per queue with a durable pull consumer
 *   (AckWait = visibility timeout, MaxDeliver = max_retries + 1).
 *   Exhausted messages are terminated and republished to a companion
 *   `<name>_DEAD` stream.
 *
 * Nothing here is enabled by default; an app opts in by creating a queue
 * handle and submitting jobs to it.
 */

#ifndef __CWIST_DURABLE_QUEUE_H__
#define __CWIST_DURABLE_QUEUE_H__

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_redis cwist_redis_t;
typedef struct cwist_nats cwist_nats_t;

/** Opaque durable queue handle. */
typedef struct cwist_job_queue cwist_job_queue_t;

/** A single claimed job. Valid until ack/nack/cwist_job_destroy. */
typedef struct cwist_job cwist_job_t;

/** Queue creation options. */
typedef struct cwist_job_queue_opts {
    /** Queue name. Required. Used as the Redis key prefix
     * (`cwist:jq:<name>:*`) and the JetStream stream name. */
    const char *name;
    /** Worker identity within the consumer group. NULL picks a unique name
     * (Redis: `w<pid>-<n>`; NATS: ephemeral durable consumer suffix). */
    const char *consumer;
    /** Time after which an unacknowledged job becomes claimable again.
     * Default 30000 ms. */
    uint64_t visibility_timeout_ms;
    /** Deliveries (including the first) before a job is dead-lettered.
     * Must be >= 1. Default 5. */
    uint32_t max_retries;
    /** Delay before a nacked job becomes claimable again. Default 0
     * (immediate). */
    uint64_t retry_delay_ms;
} cwist_job_queue_opts_t;

/**
 * @brief Create a durable queue over an existing Redis connection.
 *
 * Ensures the consumer group exists (created on first use, MKSTREAM).
 * Takes no ownership of @p conn; the connection must outlive the queue.
 */
cwist_job_queue_t *cwist_job_queue_create_redis(cwist_redis_t *conn,
                                                const cwist_job_queue_opts_t *opts);

/**
 * @brief Create a durable queue over an existing NATS connection.
 *
 * Requires a JetStream-enabled server. Ensures the job stream and a durable
 * pull consumer exist. Takes no ownership of @p nats; the connection must
 * outlive the queue.
 */
cwist_job_queue_t *cwist_job_queue_create_nats(cwist_nats_t *nats,
                                               const cwist_job_queue_opts_t *opts);

/** Destroy the queue handle. Claimed-but-unacked jobs are reclaimed by
 * other workers once their visibility timeout expires. */
void cwist_job_queue_destroy(cwist_job_queue_t *q);

/**
 * @brief Persist a job for later processing.
 *
 * @param payload Opaque payload bytes; binary-safe.
 * @param type    Optional type/tag string (NULL for none).
 */
cwist_error_t cwist_job_queue_enqueue(cwist_job_queue_t *q, const void *payload, size_t payload_len,
                                      const char *type);

/**
 * @brief Claim one job, waiting up to @p timeout_ms.
 *
 * The caller must eventually cwist_job_queue_ack(), cwist_job_queue_nack(),
 * or cwist_job_destroy() the returned job; an unacked job is reclaimed
 * after the visibility timeout.
 *
 * @return 0 on success, CWIST_ERROR_TIMEOUT when no job appears within
 * @p timeout_ms (0 = single poll).
 */
cwist_error_t cwist_job_queue_claim(cwist_job_queue_t *q, uint64_t timeout_ms, cwist_job_t **job);

/** Permanently remove a claimed job. */
cwist_error_t cwist_job_queue_ack(cwist_job_queue_t *q, cwist_job_t *job);

/**
 * @brief Return a claimed job to the queue.
 *
 * The job becomes claimable again after retry_delay_ms; once deliveries
 * exceed max_retries it is dead-lettered instead.
 */
cwist_error_t cwist_job_queue_nack(cwist_job_queue_t *q, cwist_job_t *job);

/** Release a job handle without ack/nack (the job is reclaimed later).
 * Always safe on NULL. */
void cwist_job_destroy(cwist_job_t *job);

/** Job payload bytes (binary-safe); *@p len receives the length. */
const void *cwist_job_payload(const cwist_job_t *job, size_t *len);

/** Job type/tag, or NULL when the job was enqueued without one. */
const char *cwist_job_type(const cwist_job_t *job);

/** Backend delivery token (Redis stream entry id / NATS sequence). */
const char *cwist_job_id(const cwist_job_t *job);

/** Number of deliveries so far, including the current one. */
uint32_t cwist_job_attempts(const cwist_job_t *job);

/** Number of jobs currently parked in the dead-letter store. */
cwist_error_t cwist_job_queue_dead_count(cwist_job_queue_t *q, uint64_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_DURABLE_QUEUE_H__ */
