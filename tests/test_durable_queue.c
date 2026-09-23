/**
 * @file test_durable_queue.c
 * @brief Tests for the experimental durable job queue (Redis + NATS backends).
 *
 * Redis: requires a server at CWIST_REDIS_HOST:CWIST_REDIS_PORT (default
 * 127.0.0.1:6379); skips cleanly when absent.
 * NATS: requires a JetStream-enabled server at CWIST_NATS_URL (default
 * nats://127.0.0.1:4222); skips cleanly when absent or not JetStream.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/nats/cwist_nats.h>
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/sys/job/durable_queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "[durable_queue] FAIL: %s\n", msg); \
            return -1;                                          \
        }                                                       \
    } while (0)

static void sleep_ms(uint64_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static unsigned int name_seq = 0;

static void make_name(char *buf, size_t len, const char *tag) {
    snprintf(buf, len, "dqtest-%s-%d-%ld-%u", tag, (int)getpid(), (long)time(NULL), ++name_seq);
}

/* enqueue -> claim -> ack; payload (binary) and type round-trip; ack removes. */
static int t_enqueue_claim_ack(cwist_job_queue_t *q) {
    const char binary[] = {'h', '\0', 'l', '\0', 'o'};
    cwist_error_t err = cwist_job_queue_enqueue(q, binary, sizeof(binary), "email");
    CHECK(err.error.err_i16 == 0, "enqueue failed");

    cwist_job_t *job = NULL;
    err = cwist_job_queue_claim(q, 5000, &job);
    CHECK(err.error.err_i16 == 0 && job, "claim after enqueue failed");
    size_t len = 0;
    const void *payload = cwist_job_payload(job, &len);
    CHECK(len == sizeof(binary) && memcmp(payload, binary, len) == 0, "payload round-trip failed");
    CHECK(cwist_job_type(job) && strcmp(cwist_job_type(job), "email") == 0,
          "type round-trip failed");
    CHECK(cwist_job_attempts(job) == 1, "first delivery attempts != 1");

    err = cwist_job_queue_ack(q, job);
    CHECK(err.error.err_i16 == 0, "ack failed");
    err = cwist_job_queue_claim(q, 300, &job);
    CHECK(err.error.err_i16 == CWIST_ERROR_TIMEOUT && !job, "job still claimable after ack");
    return 0;
}

/* nack -> redelivered with incremented attempt count. */
static int t_nack_redelivers(cwist_job_queue_t *q) {
    cwist_error_t err = cwist_job_queue_enqueue(q, "payload-A", 9, NULL);
    CHECK(err.error.err_i16 == 0, "enqueue failed");

    cwist_job_t *job = NULL;
    err = cwist_job_queue_claim(q, 5000, &job);
    CHECK(err.error.err_i16 == 0 && job, "claim failed");
    CHECK(cwist_job_type(job) == NULL, "type should be NULL when enqueued without one");
    err = cwist_job_queue_nack(q, job);
    CHECK(err.error.err_i16 == 0, "nack failed");

    err = cwist_job_queue_claim(q, 5000, &job);
    CHECK(err.error.err_i16 == 0 && job, "redelivery failed");
    CHECK(cwist_job_attempts(job) == 2, "redelivered attempts != 2");
    size_t len = 0;
    const void *payload = cwist_job_payload(job, &len);
    CHECK(len == 9 && memcmp(payload, "payload-A", 9) == 0, "redelivered payload mismatch");
    err = cwist_job_queue_ack(q, job);
    CHECK(err.error.err_i16 == 0, "ack after redelivery failed");
    return 0;
}

/* visibility timeout: an unacked job becomes claimable again. */
static int t_visibility_timeout(cwist_job_queue_t *q, uint64_t visibility_ms) {
    cwist_error_t err = cwist_job_queue_enqueue(q, "payload-B", 9, NULL);
    CHECK(err.error.err_i16 == 0, "enqueue failed");

    cwist_job_t *job = NULL;
    err = cwist_job_queue_claim(q, 5000, &job);
    CHECK(err.error.err_i16 == 0 && job, "first claim failed");
    cwist_job_destroy(job); /* drop without ack: simulates a crashed worker */

    sleep_ms(visibility_ms + 400);

    job = NULL;
    err = cwist_job_queue_claim(q, 10000, &job);
    CHECK(err.error.err_i16 == 0 && job, "timeout redelivery failed");
    size_t len = 0;
    const void *payload = cwist_job_payload(job, &len);
    CHECK(len == 9 && memcmp(payload, "payload-B", 9) == 0, "timeout redelivery payload mismatch");
    CHECK(cwist_job_attempts(job) == 2, "timeout redelivery attempts != 2");
    err = cwist_job_queue_ack(q, job);
    CHECK(err.error.err_i16 == 0, "ack after timeout redelivery failed");
    return 0;
}

/* retries exhausted -> dead-lettered; queue drains; dead count increments. */
static int t_dead_letter(cwist_job_queue_t *q, uint32_t max_retries) {
    uint64_t before = 0;
    cwist_error_t err = cwist_job_queue_dead_count(q, &before);
    CHECK(err.error.err_i16 == 0, "dead_count failed");

    err = cwist_job_queue_enqueue(q, "poison", 6, "poison-type");
    CHECK(err.error.err_i16 == 0, "enqueue failed");

    /* Claim + nack until the job stops coming back. */
    for (uint32_t i = 0; i < max_retries + 1; i++) {
        cwist_job_t *job = NULL;
        err = cwist_job_queue_claim(q, 5000, &job);
        if (err.error.err_i16 == CWIST_ERROR_TIMEOUT) break;
        CHECK(err.error.err_i16 == 0 && job, "claim during retry loop failed");
        err = cwist_job_queue_nack(q, job);
        CHECK(err.error.err_i16 == 0, "nack during retry loop failed");
    }

    cwist_job_t *job = NULL;
    err = cwist_job_queue_claim(q, 1000, &job);
    CHECK(err.error.err_i16 == CWIST_ERROR_TIMEOUT && !job, "dead-lettered job still claimable");

    uint64_t after = 0;
    err = cwist_job_queue_dead_count(q, &after);
    CHECK(err.error.err_i16 == 0, "dead_count failed");
    CHECK(after == before + 1, "dead count did not increase by 1");
    return 0;
}

/* Runs all scenarios against fresh queues. Returns 0 pass, -1 fail, -2 when
 * the backend could not create a queue (server absent / not JetStream). */
static int run_suite(cwist_job_queue_t *(*create)(void *ctx, const char *name,
                                                  const cwist_job_queue_opts_t *opts),
                     void *ctx, const char *tag) {
    char name[96];
    cwist_job_queue_t *q;

    make_name(name, sizeof(name), tag);
    q = create(ctx, name, &(cwist_job_queue_opts_t){.name = name});
    if (!q) return -2;
    if (t_enqueue_claim_ack(q) != 0) return -1;
    cwist_job_queue_destroy(q);

    make_name(name, sizeof(name), tag);
    q = create(ctx, name, &(cwist_job_queue_opts_t){.name = name});
    if (!q) return -2;
    if (t_nack_redelivers(q) != 0) return -1;
    cwist_job_queue_destroy(q);

    make_name(name, sizeof(name), tag);
    q = create(ctx, name, &(cwist_job_queue_opts_t){.name = name, .visibility_timeout_ms = 500});
    if (!q) return -2;
    if (t_visibility_timeout(q, 500) != 0) return -1;
    cwist_job_queue_destroy(q);

    make_name(name, sizeof(name), tag);
    q = create(ctx, name,
               &(cwist_job_queue_opts_t){.name = name, .max_retries = 2, .retry_delay_ms = 50});
    if (!q) return -2;
    if (t_dead_letter(q, 2) != 0) return -1;
    cwist_job_queue_destroy(q);

    return 0;
}

/* --- Backend factories -------------------------------------------------- */

typedef struct {
    cwist_redis_t *conn;
} redis_ctx_t;

static cwist_job_queue_t *create_redis(void *ctx, const char *name,
                                       const cwist_job_queue_opts_t *opts) {
    (void)name;
    return cwist_job_queue_create_redis(((redis_ctx_t *)ctx)->conn, opts);
}

typedef struct {
    cwist_nats_t *nats;
} nats_ctx_t;

static cwist_job_queue_t *create_nats(void *ctx, const char *name,
                                      const cwist_job_queue_opts_t *opts) {
    (void)name;
    return cwist_job_queue_create_nats(((nats_ctx_t *)ctx)->nats, opts);
}

int main(void) {
    int rc = 0;

    /* Redis backend. */
    const char *host = getenv("CWIST_REDIS_HOST") ? getenv("CWIST_REDIS_HOST") : "127.0.0.1";
    int port = getenv("CWIST_REDIS_PORT") ? atoi(getenv("CWIST_REDIS_PORT")) : 6379;
    cwist_redis_t *conn = cwist_redis_connect(host, port);
    if (!conn) {
        printf("[durable_queue] No Redis server at %s:%d, skipping redis backend.\n", host, port);
    } else {
        redis_ctx_t ctx = {conn};
        int r = run_suite(create_redis, &ctx, "redis");
        if (r != 0) {
            fprintf(stderr, "[durable_queue] Redis backend tests failed (%d).\n", r);
            rc = 1;
        } else {
            printf("[durable_queue] Redis backend tests passed.\n");
        }
        cwist_redis_close(conn);
    }

    /* NATS backend (requires JetStream). */
    const char *url = getenv("CWIST_NATS_URL") ? getenv("CWIST_NATS_URL") : "nats://127.0.0.1:4222";
    cwist_nats_t *nats = NULL;
    cwist_error_t err = cwist_nats_connect(&nats, url);
    if (err.error.err_i16 != 0 || !nats) {
        printf("[durable_queue] No NATS server at %s, skipping nats backend.\n", url);
    } else {
        nats_ctx_t ctx = {nats};
        int r = run_suite(create_nats, &ctx, "nats");
        if (r == -2) {
            printf("[durable_queue] NATS server at %s has no JetStream, skipping nats backend.\n",
                   url);
        } else if (r != 0) {
            fprintf(stderr, "[durable_queue] NATS backend tests failed (%d).\n", r);
            rc = 1;
        } else {
            printf("[durable_queue] NATS backend tests passed.\n");
        }
        cwist_nats_destroy(nats);
    }

    if (rc == 0) printf("All durable queue tests passed (skipped backends logged above).\n");
    return rc;
}
