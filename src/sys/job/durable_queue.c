/**
 * @file durable_queue.c
 * @brief Durable job queue backends over Redis (streams) and NATS (JetStream).
 *
 * EXPERIMENTAL (v3.7 Phase 4). See include/cwist/sys/job/durable_queue.h.
 *
 * Redis layout (prefix `cwist:jq:<name>:`):
 * - `<prefix>s`   stream; entries carry `type` + `payload` fields
 * - `<prefix>a`   hash: entry id -> delivery count (visibility-timeout
 *                 redeliveries increment it; ack/nack/dead-letter remove it)
 * - `<prefix>delay` zset: nacked jobs parked until their due time; the blob
 *                 is [type_len:4BE][attempts:4BE][type][payload]
 * - `<prefix>dead`  list: dead-lettered blobs
 *
 * Claim order per poll: reap due delayed jobs (Lua, atomic), reclaim stale
 * pending entries via XAUTOCLAIM (visibility timeout), then read new
 * entries via XREADGROUP. Dead-lettering happens when the delivery count
 * exceeds max_retries.
 *
 * NATS layout: JetStream stream `<name>` (subject `<name>.jobs`) with a
 * durable pull consumer (AckWait = visibility, MaxDeliver = max_retries+1
 * as headroom). Exhausted deliveries are terminated and republished to a
 * companion `<name>_DEAD` stream, so both backends expose a dead count.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/job/durable_queue.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/nats/cwist_nats.h>
#include <cwist/net/redis/cwist_redis.h>
#include <nats.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CWIST_JQ_GROUP "cwist-jq"
#define CWIST_JQ_DEFAULT_VISIBILITY_MS 30000u
#define CWIST_JQ_DEFAULT_MAX_RETRIES 5u
#define CWIST_JQ_MAX_POLL_SLICE_MS 200u /* max single XREADGROUP BLOCK */

/* Atomically schedules a nacked job: XACK, then either park it in the
 * delay zset or dead-letter it. Returns 1 (delayed), 2 (dead-lettered),
 * 0 (entry no longer pending). */
static const char *JQ_NACK_SCRIPT = "local ok = redis.call('XACK', KEYS[1], KEYS[2], ARGV[1])\n"
                                    "if ok == 0 then return 0 end\n"
                                    "redis.call('HDEL', KEYS[5], ARGV[1])\n"
                                    "if tonumber(ARGV[2]) > tonumber(ARGV[3]) then\n"
                                    "  redis.call('RPUSH', KEYS[3], ARGV[4])\n"
                                    "  return 2\n"
                                    "end\n"
                                    "redis.call('ZADD', KEYS[4], ARGV[5], ARGV[4])\n"
                                    "return 1\n";

/* Moves due delayed blobs back onto the stream. The attempts hash is
 * pre-seeded with attempts-1 because the fresh-claim path HINCRBYs. */
static const char *JQ_REAP_SCRIPT =
    "local items = redis.call('ZRANGEBYSCORE', KEYS[1], '-inf', ARGV[1], "
    "'LIMIT', 0, 128)\n"
    "for _, blob in ipairs(items) do\n"
    "  local tl = string.byte(blob,1)*16777216 + string.byte(blob,2)*65536 + "
    "string.byte(blob,3)*256 + string.byte(blob,4)\n"
    "  local at = string.byte(blob,5)*16777216 + string.byte(blob,6)*65536 + "
    "string.byte(blob,7)*256 + string.byte(blob,8)\n"
    "  local id = redis.call('XADD', KEYS[2], '*', 'type', "
    "string.sub(blob, 9, 8 + tl), 'payload', string.sub(blob, 9 + tl))\n"
    "  redis.call('HSET', KEYS[3], id, at - 1)\n"
    "  redis.call('ZREM', KEYS[1], blob)\n"
    "end\n"
    "return #items\n";

/* Ensures the consumer group exists; 1 = created, 2 = already existed,
 * 0 = real error. */
static const char *JQ_GROUP_SCRIPT =
    "local ok, err = pcall(redis.call, 'XGROUP', 'CREATE', KEYS[1], ARGV[1], "
    "'$', 'MKSTREAM')\n"
    "if ok then return 1 end\n"
    "if string.match(err, 'BUSYGROUP') then return 2 end\n"
    "return 0\n";

typedef cwist_error_t (*jq_dead_count_fn)(cwist_job_queue_t *q, uint64_t *out_count);

static cwist_error_t jq_nats_dead_count(cwist_job_queue_t *q, uint64_t *out_count);

typedef struct {
    cwist_redis_t *conn; /* borrowed */
    char *stream;
    char *attempts;
    char *dead;
    char *delay;
    char *consumer; /* owned copy */
} jq_redis_t;

typedef struct {
    natsConnection *conn; /* borrowed */
    jsCtx *js;
    natsSubscription *sub;
    char *subject;      /* <base>.jobs */
    char *dead_subject; /* <base>.dead */
    char *dead_stream;  /* <base>_DEAD */
} jq_nats_t;

struct cwist_job_queue {
    int backend; /* 0 = redis, 1 = nats */
    char *name;
    char *consumer;
    uint64_t visibility_timeout_ms;
    uint32_t max_retries;
    uint64_t retry_delay_ms;
    jq_dead_count_fn dead_count;
    union {
        jq_redis_t redis;
        jq_nats_t nats;
    } u;
};

struct cwist_job {
    cwist_job_queue_t *q;
    void *backend; /* natsMsg* for the NATS backend, NULL for redis */
    char *id;
    char *type; /* NULL or owned copy */
    unsigned char *payload;
    size_t payload_len;
    uint32_t attempts;
};

static cwist_error_t jq_err(int16_t code) {
    cwist_error_t e = make_error(CWIST_ERR_INT16);
    e.error.err_i16 = code;
    return e;
}

static uint64_t jq_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static unsigned int jq_consumer_seq = 0;

static char *jq_default_consumer(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "w%d-%u", (int)getpid(), ++jq_consumer_seq);
    return cwist_strdup(buf);
}

/** Copy opts with defaults filled in; returns false on invalid/ENOMEM. */
static bool jq_opts_init(cwist_job_queue_t *q, const cwist_job_queue_opts_t *opts) {
    if (!opts || !opts->name || !*opts->name) return false;
    q->name = cwist_strdup(opts->name);
    q->consumer =
        opts->consumer && *opts->consumer ? cwist_strdup(opts->consumer) : jq_default_consumer();
    q->visibility_timeout_ms =
        opts->visibility_timeout_ms ? opts->visibility_timeout_ms : CWIST_JQ_DEFAULT_VISIBILITY_MS;
    q->max_retries = opts->max_retries ? opts->max_retries : CWIST_JQ_DEFAULT_MAX_RETRIES;
    q->retry_delay_ms = opts->retry_delay_ms;
    if (!q->name || !q->consumer) return false;
    return true;
}

static void cwist_job_free(cwist_job_t *job) {
    if (!job) return;
    if (job->backend) natsMsg_Destroy((natsMsg *)job->backend);
    cwist_free(job->id);
    cwist_free(job->type);
    cwist_free(job->payload);
    cwist_free(job);
}

void cwist_job_destroy(cwist_job_t *job) {
    cwist_job_free(job);
}

const void *cwist_job_payload(const cwist_job_t *job, size_t *len) {
    if (!job) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = job->payload_len;
    return job->payload;
}

const char *cwist_job_type(const cwist_job_t *job) {
    return job ? job->type : NULL;
}

const char *cwist_job_id(const cwist_job_t *job) {
    return job ? job->id : NULL;
}

uint32_t cwist_job_attempts(const cwist_job_t *job) {
    return job ? job->attempts : 0;
}

/* --- Redis backend ------------------------------------------------------ */

static char *jq_key(const char *name, const char *suffix) {
    size_t n = strlen(name) + strlen(suffix) + 16;
    char *key = cwist_alloc(n);
    if (key) snprintf(key, n, "cwist:jq:%s:%s", name, suffix);
    return key;
}

static cwist_error_t jq_redis_cmd(cwist_redis_t *conn, size_t argc, const void *const *argv,
                                  const size_t *argv_lens, cwist_redis_reply_t **reply) {
    cwist_error_t err = cwist_redis_command_argv_reply(conn, argc, argv, argv_lens, reply);
    if (!cwist_error_is_ok(&err) || !*reply) {
        cwist_redis_reply_free(*reply);
        *reply = NULL;
        return jq_err(CWIST_FAILURE);
    }
    return jq_err(0);
}

/** Run a command whose reply we only need the integer value of. */
static cwist_error_t jq_redis_int(cwist_redis_t *conn, size_t argc, const void *const *argv,
                                  const size_t *argv_lens, long long *out) {
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(conn, argc, argv, argv_lens, &reply);
    if (!cwist_error_is_ok(&err)) return err;
    if (reply->type == ':' || reply->type == '$')
        *out = reply->integer ? reply->integer : strtoll(reply->str ? reply->str : "0", NULL, 10);
    else
        *out = 0;
    cwist_redis_reply_free(reply);
    return jq_err(0);
}

/** Pack [type_len:4BE][attempts:4BE][type][payload] into an owned blob. */
static char *jq_blob(const char *type, uint32_t attempts, const void *payload, size_t payload_len,
                     size_t *out_len) {
    uint32_t type_len = type ? (uint32_t)strlen(type) : 0;
    size_t len = 8 + type_len + payload_len;
    char *blob = cwist_alloc(len ? len : 1);
    if (!blob) return NULL;
    blob[0] = (char)(type_len >> 24);
    blob[1] = (char)(type_len >> 16);
    blob[2] = (char)(type_len >> 8);
    blob[3] = (char)type_len;
    blob[4] = (char)(attempts >> 24);
    blob[5] = (char)(attempts >> 16);
    blob[6] = (char)(attempts >> 8);
    blob[7] = (char)attempts;
    if (type_len) memcpy(blob + 8, type, type_len);
    if (payload_len) memcpy(blob + 8 + type_len, payload, payload_len);
    *out_len = len;
    return blob;
}

static cwist_error_t jq_redis_enqueue(cwist_job_queue_t *q, const void *payload, size_t payload_len,
                                      const char *type) {
    jq_redis_t *rs = &q->u.redis;
    const char empty[] = "";
    if (!type) type = empty;
    const void *argv[] = {"XADD", rs->stream, "*", "type", type, "payload", payload};
    size_t lens[] = {4, strlen(rs->stream), 1, 4, strlen(type), 7, payload_len};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(rs->conn, 7, argv, lens, &reply);
    cwist_redis_reply_free(reply);
    return err;
}

/** Look up a field in a stream entry's [k1, v1, k2, v2, ...] array. */
static const cwist_redis_reply_t *jq_entry_field(const cwist_redis_reply_t *fields,
                                                 const char *name) {
    if (!fields || fields->type != '*') return NULL;
    for (size_t i = 0; i + 1 < fields->elements; i += 2) {
        const cwist_redis_reply_t *k = fields->element[i];
        if (k && k->str && strcmp(k->str, name) == 0) return fields->element[i + 1];
    }
    return NULL;
}

static cwist_job_t *jq_job_from_entry(cwist_job_queue_t *q, const cwist_redis_reply_t *entry) {
    if (!entry || entry->type != '*' || entry->elements != 2 || !entry->element[0] ||
        !entry->element[0]->str)
        return NULL;
    const cwist_redis_reply_t *id = entry->element[0];
    const cwist_redis_reply_t *fields = entry->element[1];
    const cwist_redis_reply_t *type = jq_entry_field(fields, "type");
    const cwist_redis_reply_t *payload = jq_entry_field(fields, "payload");

    cwist_job_t *job = cwist_alloc(sizeof(*job));
    if (!job) return NULL;
    memset(job, 0, sizeof(*job));
    job->q = q;
    job->id = cwist_strndup(id->str, id->len);
    if (type && type->str && type->len) job->type = cwist_strndup(type->str, type->len);
    job->payload_len = payload && payload->str ? payload->len : 0;
    if (job->payload_len) {
        job->payload = cwist_alloc(job->payload_len);
        if (job->payload) memcpy(job->payload, payload->str, job->payload_len);
    }
    if (!job->id || (job->payload_len && !job->payload)) {
        cwist_job_free(job);
        return NULL;
    }
    return job;
}

/** Increment the delivery count for a redis entry id. */
static cwist_error_t jq_redis_delivery_count(cwist_job_queue_t *q, const char *id, size_t id_len,
                                             uint32_t *out_attempts) {
    jq_redis_t *rs = &q->u.redis;
    char one[] = "1";
    const void *argv[] = {"HINCRBY", rs->attempts, id, one};
    size_t lens[] = {7, strlen(rs->attempts), id_len, 1};
    long long n = 0;
    cwist_error_t err = jq_redis_int(rs->conn, 4, argv, lens, &n);
    if (cwist_error_is_ok(&err)) *out_attempts = n > 0 ? (uint32_t)n : 1;
    return err;
}

/** Run a fire-and-forget command; true when it succeeded. */
static bool jq_redis_run(cwist_redis_t *conn, size_t argc, const void *const *argv,
                         const size_t *argv_lens) {
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(conn, argc, argv, argv_lens, &reply);
    cwist_redis_reply_free(reply);
    return cwist_error_is_ok(&err);
}

/** Dead-letter a stream entry: park its blob and drop all bookkeeping. */
static void jq_redis_dead_letter(cwist_job_queue_t *q, const char *id, size_t id_len,
                                 const char *type, const void *payload, size_t payload_len,
                                 uint32_t attempts) {
    jq_redis_t *rs = &q->u.redis;
    size_t blob_len = 0;
    char *blob = jq_blob(type, attempts, payload, payload_len, &blob_len);
    if (blob) {
        const void *argv[] = {"RPUSH", rs->dead, blob};
        size_t lens[] = {5, strlen(rs->dead), blob_len};
        jq_redis_run(rs->conn, 3, argv, lens);
        cwist_free(blob);
    }
    const void *argv[] = {"XACK", rs->stream, CWIST_JQ_GROUP, id};
    size_t lens[] = {4, strlen(rs->stream), strlen(CWIST_JQ_GROUP), id_len};
    jq_redis_run(rs->conn, 4, argv, lens);
    const void *dargv[] = {"HDEL", rs->attempts, id};
    size_t dlens[] = {4, strlen(rs->attempts), id_len};
    jq_redis_run(rs->conn, 3, dargv, dlens);
}

/** Reclaim entries whose visibility timeout expired; may dead-letter some. */
static cwist_job_t *jq_redis_autoclaim(cwist_job_queue_t *q) {
    jq_redis_t *rs = &q->u.redis;
    char vis[32];
    int vis_len = snprintf(vis, sizeof(vis), "%llu", (unsigned long long)q->visibility_timeout_ms);
    const char *zero[] = {"0-0"};
    const char *count[] = {"128"};
    const void *argv[] = {"XAUTOCLAIM", rs->stream, CWIST_JQ_GROUP, rs->consumer,
                          vis,          zero[0],    "COUNT",        count[0]};
    size_t lens[] = {
        10, strlen(rs->stream), strlen(CWIST_JQ_GROUP), strlen(rs->consumer), (size_t)vis_len, 3, 5,
        3};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(rs->conn, 8, argv, lens, &reply);
    if (!cwist_error_is_ok(&err) || !reply || reply->type != '*' || reply->elements < 2) {
        cwist_redis_reply_free(reply);
        return NULL;
    }
    cwist_job_t *job = NULL;
    const cwist_redis_reply_t *entries = reply->element[1];
    if (entries && entries->type == '*') {
        for (size_t i = 0; i < entries->elements && !job; i++) {
            cwist_job_t *candidate = jq_job_from_entry(q, entries->element[i]);
            if (!candidate) continue;
            uint32_t attempts = 0;
            cwist_error_t derr =
                jq_redis_delivery_count(q, candidate->id, strlen(candidate->id), &attempts);
            if (!cwist_error_is_ok(&derr) || attempts <= q->max_retries) {
                candidate->attempts = attempts;
                job = candidate;
            } else {
                jq_redis_dead_letter(q, candidate->id, strlen(candidate->id), candidate->type,
                                     candidate->payload, candidate->payload_len, attempts);
                cwist_job_free(candidate);
            }
        }
    }
    cwist_redis_reply_free(reply);
    return job;
}

/** Read one new entry (BLOCK up to block_ms). */
static cwist_job_t *jq_redis_readgroup(cwist_job_queue_t *q, uint64_t block_ms) {
    jq_redis_t *rs = &q->u.redis;
    char block[32];
    int block_len = snprintf(block, sizeof(block), "%llu", (unsigned long long)block_ms);
    const char gt[] = ">";
    const void *argv[] = {"XREADGROUP", "GROUP", CWIST_JQ_GROUP, rs->consumer, "COUNT", "1",
                          "BLOCK",      block,   "STREAMS",      rs->stream,   gt};
    size_t lens[] = {10,
                     5,
                     strlen(CWIST_JQ_GROUP),
                     strlen(rs->consumer),
                     5,
                     1,
                     5,
                     (size_t)block_len,
                     7,
                     strlen(rs->stream),
                     1};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(rs->conn, 11, argv, lens, &reply);
    if (!cwist_error_is_ok(&err) || !reply || reply->type != '*' || reply->elements < 1) {
        cwist_redis_reply_free(reply);
        return NULL;
    }
    cwist_job_t *job = NULL;
    const cwist_redis_reply_t *top = reply->element[0];
    if (top && top->type == '*' && top->elements == 2 && top->element[1] &&
        top->element[1]->type == '*') {
        const cwist_redis_reply_t *entries = top->element[1];
        for (size_t i = 0; i < entries->elements && !job; i++) {
            cwist_job_t *candidate = jq_job_from_entry(q, entries->element[i]);
            if (!candidate) continue;
            uint32_t attempts = 0;
            cwist_error_t derr =
                jq_redis_delivery_count(q, candidate->id, strlen(candidate->id), &attempts);
            if (cwist_error_is_ok(&derr)) {
                candidate->attempts = attempts;
                job = candidate;
            } else {
                cwist_job_free(candidate);
            }
        }
    }
    cwist_redis_reply_free(reply);
    return job;
}

static void jq_redis_reap_delay(cwist_job_queue_t *q) {
    jq_redis_t *rs = &q->u.redis;
    char now[32];
    int now_len = snprintf(now, sizeof(now), "%llu", (unsigned long long)jq_now_ms());
    const char three[] = "3";
    const void *argv[] = {"EVAL", JQ_REAP_SCRIPT, three, rs->delay, rs->stream, rs->attempts, now};
    size_t lens[] = {4,
                     strlen(JQ_REAP_SCRIPT),
                     1,
                     strlen(rs->delay),
                     strlen(rs->stream),
                     strlen(rs->attempts),
                     (size_t)now_len};
    jq_redis_run(rs->conn, 7, argv, lens);
}

static cwist_error_t jq_redis_claim(cwist_job_queue_t *q, uint64_t timeout_ms, cwist_job_t **job) {
    uint64_t deadline = jq_now_ms() + timeout_ms;
    bool polled = false;
    for (;;) {
        jq_redis_reap_delay(q);
        cwist_job_t *found = jq_redis_autoclaim(q);
        if (found) {
            *job = found;
            return jq_err(0);
        }
        uint64_t now = jq_now_ms();
        if (polled && now >= deadline) return jq_err(CWIST_ERROR_TIMEOUT);
        polled = true;
        uint64_t block = deadline > now ? deadline - now : 1;
        if (block > CWIST_JQ_MAX_POLL_SLICE_MS) block = CWIST_JQ_MAX_POLL_SLICE_MS;
        found = jq_redis_readgroup(q, block);
        if (found) {
            *job = found;
            return jq_err(0);
        }
        if (jq_now_ms() >= deadline) return jq_err(CWIST_ERROR_TIMEOUT);
    }
}

static cwist_error_t jq_redis_ack(cwist_job_queue_t *q, cwist_job_t *job) {
    jq_redis_t *rs = &q->u.redis;
    const void *argv[] = {"XACK", rs->stream, CWIST_JQ_GROUP, job->id};
    size_t lens[] = {4, strlen(rs->stream), strlen(CWIST_JQ_GROUP), strlen(job->id)};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(rs->conn, 4, argv, lens, &reply);
    cwist_redis_reply_free(reply);
    const void *dargv[] = {"HDEL", rs->attempts, job->id};
    size_t dlens[] = {4, strlen(rs->attempts), strlen(job->id)};
    jq_redis_run(rs->conn, 3, dargv, dlens);
    return err;
}

static cwist_error_t jq_redis_nack(cwist_job_queue_t *q, cwist_job_t *job) {
    jq_redis_t *rs = &q->u.redis;
    uint32_t next = job->attempts + 1;
    uint64_t due = jq_now_ms() + q->retry_delay_ms;
    char next_buf[16], max_buf[16], due_buf[32];
    int next_len = snprintf(next_buf, sizeof(next_buf), "%u", next);
    int max_len = snprintf(max_buf, sizeof(max_buf), "%u", q->max_retries);
    int due_len = snprintf(due_buf, sizeof(due_buf), "%llu", (unsigned long long)due);
    size_t blob_len = 0;
    char *blob = jq_blob(job->type, next, job->payload, job->payload_len, &blob_len);
    if (!blob) return jq_err(CWIST_ERROR_NOMEM);
    const char five[] = "5";
    const void *argv[] = {"EVAL",   JQ_NACK_SCRIPT, five,         rs->stream, CWIST_JQ_GROUP,
                          rs->dead, rs->delay,      rs->attempts, job->id,    next_buf,
                          max_buf,  blob,           due_buf};
    size_t lens[] = {4,
                     strlen(JQ_NACK_SCRIPT),
                     1,
                     strlen(rs->stream),
                     strlen(CWIST_JQ_GROUP),
                     strlen(rs->dead),
                     strlen(rs->delay),
                     strlen(rs->attempts),
                     strlen(job->id),
                     (size_t)next_len,
                     (size_t)max_len,
                     blob_len,
                     (size_t)due_len};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = jq_redis_cmd(rs->conn, 13, argv, lens, &reply);
    cwist_redis_reply_free(reply);
    cwist_free(blob);
    return err;
}

static cwist_error_t jq_redis_dead_count(cwist_job_queue_t *q, uint64_t *out_count) {
    jq_redis_t *rs = &q->u.redis;
    const void *argv[] = {"LLEN", rs->dead};
    size_t lens[] = {4, strlen(rs->dead)};
    long long n = 0;
    cwist_error_t err = jq_redis_int(rs->conn, 2, argv, lens, &n);
    if (cwist_error_is_ok(&err)) *out_count = n > 0 ? (uint64_t)n : 0;
    return err;
}

cwist_job_queue_t *cwist_job_queue_create_redis(cwist_redis_t *conn,
                                                const cwist_job_queue_opts_t *opts) {
    if (!conn) return NULL;
    cwist_job_queue_t *q = cwist_alloc(sizeof(*q));
    if (!q) return NULL;
    memset(q, 0, sizeof(*q));
    if (!jq_opts_init(q, opts)) goto fail;
    q->backend = 0;
    q->dead_count = jq_redis_dead_count;
    jq_redis_t *rs = &q->u.redis;
    rs->conn = conn;
    rs->stream = jq_key(q->name, "s");
    rs->attempts = jq_key(q->name, "a");
    rs->dead = jq_key(q->name, "dead");
    rs->delay = jq_key(q->name, "delay");
    rs->consumer = cwist_strdup(q->consumer);
    if (!rs->stream || !rs->attempts || !rs->dead || !rs->delay || !rs->consumer) goto fail;

    const void *argv[] = {"EVAL", JQ_GROUP_SCRIPT, "1", rs->stream, CWIST_JQ_GROUP};
    size_t lens[] = {4, strlen(JQ_GROUP_SCRIPT), 1, strlen(rs->stream), strlen(CWIST_JQ_GROUP)};
    long long created = 0;
    cwist_error_t gerr = jq_redis_int(conn, 5, argv, lens, &created);
    if (!cwist_error_is_ok(&gerr) || created == 0) goto fail;
    return q;

fail:
    if (q) {
        if (q->u.redis.stream) cwist_free(q->u.redis.stream);
        if (q->u.redis.attempts) cwist_free(q->u.redis.attempts);
        if (q->u.redis.dead) cwist_free(q->u.redis.dead);
        if (q->u.redis.delay) cwist_free(q->u.redis.delay);
        if (q->u.redis.consumer) cwist_free(q->u.redis.consumer);
        cwist_free(q->name);
        cwist_free(q->consumer);
        cwist_free(q);
    }
    return NULL;
}

/* --- NATS backend ------------------------------------------------------- */

/** [A-Za-z0-9_-] only; everything else becomes '_' (stream/consumer name and
 * subject-safe base). */
static char *jq_sanitize(const char *name) {
    size_t len = strlen(name);
    char *out = cwist_alloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        out[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                         c == '-' || c == '_'
                     ? c
                     : '_';
    }
    out[len] = '\0';
    return out;
}

static cwist_error_t jq_nats_ensure_stream(jq_nats_t *ns, const char *stream, const char *subject) {
    jsErrCode jerr = 0;
    jsStreamInfo *si = NULL;
    if (js_GetStreamInfo(&si, ns->js, stream, NULL, &jerr) == NATS_OK) {
        jsStreamInfo_Destroy(si);
        return jq_err(0);
    }
    const char *subjects[] = {subject};
    jsStreamConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.Name = stream;
    cfg.Subjects = subjects;
    cfg.SubjectsLen = 1;
    cfg.Storage = js_FileStorage;
    if (js_AddStream(&si, ns->js, &cfg, NULL, &jerr) != NATS_OK) return jq_err(CWIST_FAILURE);
    jsStreamInfo_Destroy(si);
    return jq_err(0);
}

cwist_job_queue_t *cwist_job_queue_create_nats(cwist_nats_t *nats,
                                               const cwist_job_queue_opts_t *opts) {
    if (!nats) return NULL;
    natsConnection *nc = cwist_nats_native(nats);
    if (!nc) return NULL;
    cwist_job_queue_t *q = cwist_alloc(sizeof(*q));
    if (!q) return NULL;
    memset(q, 0, sizeof(*q));
    if (!jq_opts_init(q, opts)) goto fail;
    q->backend = 1;
    q->dead_count = jq_nats_dead_count;
    jq_nats_t *ns = &q->u.nats;
    ns->conn = nc;
    char *base = jq_sanitize(q->name);
    char *consumer = jq_sanitize(q->consumer);
    if (!base || !consumer) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    }
    size_t n = strlen(base);
    ns->subject = cwist_alloc(n + 6);
    ns->dead_subject = cwist_alloc(n + 6);
    ns->dead_stream = cwist_alloc(n + 6);
    if (!ns->subject || !ns->dead_subject || !ns->dead_stream) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    }
    snprintf(ns->subject, n + 6, "%s.jobs", base);
    snprintf(ns->dead_subject, n + 6, "%s.dead", base);
    snprintf(ns->dead_stream, n + 5, "%s_DEAD", base);

    if (natsConnection_JetStream(&ns->js, nc, NULL) != NATS_OK) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    }
    cwist_error_t serr = jq_nats_ensure_stream(ns, base, ns->subject);
    if (cwist_error_is_ok(&serr))
        serr = jq_nats_ensure_stream(ns, ns->dead_stream, ns->dead_subject);
    if (!cwist_error_is_ok(&serr)) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    }

    /* Durable pull consumer; tolerate "already exists" (same config from a
     * previous run). */
    jsErrCode jerr = 0;
    jsConsumerInfo *ci = NULL;
    jsConsumerConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.Durable = consumer;
    cc.AckPolicy = js_AckExplicit;
    cc.AckWait = (int64_t)q->visibility_timeout_ms * 1000000;
    cc.MaxDeliver = (int64_t)q->max_retries + 1;
    cc.DeliverPolicy = js_DeliverAll;
    cc.FilterSubject = ns->subject;
    if (js_AddConsumer(&ci, ns->js, base, &cc, NULL, &jerr) == NATS_OK) {
        jsConsumerInfo_Destroy(ci);
    } else if (js_GetConsumerInfo(&ci, ns->js, base, consumer, NULL, &jerr) != NATS_OK) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    } else {
        jsConsumerInfo_Destroy(ci);
    }
    if (js_PullSubscribe(&ns->sub, ns->js, ns->subject, consumer, NULL, NULL, &jerr) != NATS_OK ||
        natsConnection_Flush(nc) != NATS_OK) {
        cwist_free(base);
        cwist_free(consumer);
        goto fail;
    }
    cwist_free(base);
    cwist_free(consumer);
    return q;

fail:
    if (q) {
        if (q->u.nats.sub) natsSubscription_Destroy(q->u.nats.sub);
        if (q->u.nats.js) jsCtx_Destroy(q->u.nats.js);
        cwist_free(q->u.nats.subject);
        cwist_free(q->u.nats.dead_subject);
        cwist_free(q->u.nats.dead_stream);
        cwist_free(q->name);
        cwist_free(q->consumer);
        cwist_free(q);
    }
    return NULL;
}

static cwist_error_t jq_nats_enqueue(cwist_job_queue_t *q, const void *payload, size_t payload_len,
                                     const char *type) {
    jq_nats_t *ns = &q->u.nats;
    natsMsg *msg = NULL;
    if (natsMsg_Create(&msg, ns->subject, NULL, (const char *)payload, (int)payload_len) != NATS_OK)
        return jq_err(CWIST_FAILURE);
    if (type && natsMsgHeader_Set(msg, "Cwist-Type", type) != NATS_OK) {
        natsMsg_Destroy(msg);
        return jq_err(CWIST_FAILURE);
    }
    jsErrCode jerr = 0;
    jsPubAck *pa = NULL;
    natsStatus s = js_PublishMsg(&pa, ns->js, msg, NULL, &jerr);
    if (pa) jsPubAck_Destroy(pa);
    natsMsg_Destroy(msg);
    return s == NATS_OK ? jq_err(0) : jq_err(CWIST_FAILURE);
}

/** Terminate an exhausted delivery and park it in the dead stream. */
static void jq_nats_dead_letter(jq_nats_t *ns, natsMsg *msg) {
    natsMsg *dm = NULL;
    if (natsMsg_Create(&dm, ns->dead_subject, NULL, natsMsg_GetData(msg),
                       natsMsg_GetDataLength(msg)) == NATS_OK) {
        const char *type = NULL;
        if (natsMsgHeader_Get(msg, "Cwist-Type", &type) == NATS_OK && type)
            natsMsgHeader_Set(dm, "Cwist-Type", type);
        jsErrCode jerr = 0;
        jsPubAck *pa = NULL;
        if (js_PublishMsg(&pa, ns->js, dm, NULL, &jerr) == NATS_OK && pa) jsPubAck_Destroy(pa);
        natsMsg_Destroy(dm);
    }
    natsMsg_Term(msg, NULL);
}

static cwist_error_t jq_nats_claim(cwist_job_queue_t *q, uint64_t timeout_ms, cwist_job_t **job) {
    jq_nats_t *ns = &q->u.nats;
    uint64_t deadline = jq_now_ms() + timeout_ms;
    for (;;) {
        uint64_t now = jq_now_ms();
        int64_t slice = (int64_t)(now >= deadline ? 1 : deadline - now);
        if (slice > (int64_t)CWIST_JQ_MAX_POLL_SLICE_MS)
            slice = (int64_t)CWIST_JQ_MAX_POLL_SLICE_MS;
        natsMsgList list;
        memset(&list, 0, sizeof(list));
        natsStatus s = natsSubscription_Fetch(&list, ns->sub, 1, slice, NULL);
        if (s != NATS_OK) {
            if (s == NATS_TIMEOUT) {
                if (jq_now_ms() >= deadline) return jq_err(CWIST_ERROR_TIMEOUT);
                continue;
            }
            return jq_err(CWIST_FAILURE);
        }
        for (int i = 0; i < list.Count; i++) {
            natsMsg *msg = list.Msgs[i];
            list.Msgs[i] = NULL; /* keep the list from double-destroying it */
            jsMsgMetaData *meta = NULL;
            if (natsMsg_GetMetaData(&meta, msg) != NATS_OK) {
                natsMsg_Destroy(msg);
                continue;
            }
            uint64_t delivered = meta->NumDelivered;
            if (delivered > (uint64_t)q->max_retries) {
                /* Max deliveries exhausted: park and terminate. */
                jq_nats_dead_letter(ns, msg);
                natsMsg_Destroy(msg);
                jsMsgMetaData_Destroy(meta);
                continue;
            }
            cwist_job_t *j = cwist_alloc(sizeof(*j));
            if (!j) {
                natsMsg_Destroy(msg);
                jsMsgMetaData_Destroy(meta);
                natsMsgList_Destroy(&list);
                return jq_err(CWIST_ERROR_NOMEM);
            }
            memset(j, 0, sizeof(*j));
            j->q = q;
            j->backend = msg; /* ownership moves to the job */
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)meta->Sequence.Stream);
            j->id = cwist_strdup(id_buf);
            int dlen = natsMsg_GetDataLength(msg);
            j->payload_len = dlen > 0 ? (size_t)dlen : 0;
            if (j->payload_len) {
                j->payload = cwist_alloc(j->payload_len);
                if (j->payload) memcpy(j->payload, natsMsg_GetData(msg), j->payload_len);
            }
            const char *type = NULL;
            if (natsMsgHeader_Get(msg, "Cwist-Type", &type) == NATS_OK && type && *type)
                j->type = cwist_strdup(type);
            j->attempts = (uint32_t)delivered;
            jsMsgMetaData_Destroy(meta);
            if (!j->id || (j->payload_len && !j->payload)) {
                cwist_job_free(j);
                natsMsgList_Destroy(&list);
                return jq_err(CWIST_ERROR_NOMEM);
            }
            natsMsgList_Destroy(&list); /* frees the array, not the claimed msg */
            *job = j;
            return jq_err(0);
        }
        natsMsgList_Destroy(&list);
        if (jq_now_ms() >= deadline) return jq_err(CWIST_ERROR_TIMEOUT);
    }
}

static cwist_error_t jq_nats_ack(cwist_job_queue_t *q, cwist_job_t *job) {
    (void)q;
    natsStatus s = natsMsg_Ack((natsMsg *)job->backend, NULL);
    natsMsg_Destroy((natsMsg *)job->backend);
    job->backend = NULL;
    return s == NATS_OK ? jq_err(0) : jq_err(CWIST_FAILURE);
}

static cwist_error_t jq_nats_nack(cwist_job_queue_t *q, cwist_job_t *job) {
    natsStatus s;
    if (q->retry_delay_ms)
        s = natsMsg_NakWithDelay((natsMsg *)job->backend, (int64_t)q->retry_delay_ms, NULL);
    else
        s = natsMsg_Nak((natsMsg *)job->backend, NULL);
    natsMsg_Destroy((natsMsg *)job->backend);
    job->backend = NULL;
    return s == NATS_OK ? jq_err(0) : jq_err(CWIST_FAILURE);
}

static cwist_error_t jq_nats_dead_count(cwist_job_queue_t *q, uint64_t *out_count) {
    jq_nats_t *ns = &q->u.nats;
    jsErrCode jerr = 0;
    jsStreamInfo *si = NULL;
    if (js_GetStreamInfo(&si, ns->js, ns->dead_stream, NULL, &jerr) != NATS_OK)
        return jq_err(CWIST_FAILURE);
    *out_count = (uint64_t)si->State.Msgs;
    jsStreamInfo_Destroy(si);
    return jq_err(0);
}

/* --- Dispatch ----------------------------------------------------------- */

void cwist_job_queue_destroy(cwist_job_queue_t *q) {
    if (!q) return;
    if (q->backend == 0) {
        cwist_free(q->u.redis.stream);
        cwist_free(q->u.redis.attempts);
        cwist_free(q->u.redis.dead);
        cwist_free(q->u.redis.delay);
        cwist_free(q->u.redis.consumer);
    } else {
        if (q->u.nats.sub) natsSubscription_Destroy(q->u.nats.sub);
        if (q->u.nats.js) jsCtx_Destroy(q->u.nats.js);
        cwist_free(q->u.nats.subject);
        cwist_free(q->u.nats.dead_subject);
        cwist_free(q->u.nats.dead_stream);
    }
    cwist_free(q->name);
    cwist_free(q->consumer);
    cwist_free(q);
}

cwist_error_t cwist_job_queue_enqueue(cwist_job_queue_t *q, const void *payload, size_t payload_len,
                                      const char *type) {
    if (!q || (!payload && payload_len)) return jq_err(CWIST_ERROR_INVALID_PARAM);
    if (!payload) payload = "";
    return q->backend == 0 ? jq_redis_enqueue(q, payload, payload_len, type)
                           : jq_nats_enqueue(q, payload, payload_len, type);
}

cwist_error_t cwist_job_queue_claim(cwist_job_queue_t *q, uint64_t timeout_ms, cwist_job_t **job) {
    if (!q || !job) return jq_err(CWIST_ERROR_INVALID_PARAM);
    *job = NULL;
    return q->backend == 0 ? jq_redis_claim(q, timeout_ms, job) : jq_nats_claim(q, timeout_ms, job);
}

cwist_error_t cwist_job_queue_ack(cwist_job_queue_t *q, cwist_job_t *job) {
    if (!q || !job || job->q != q) return jq_err(CWIST_ERROR_INVALID_PARAM);
    cwist_error_t err = q->backend == 0 ? jq_redis_ack(q, job) : jq_nats_ack(q, job);
    cwist_job_free(job);
    return err;
}

cwist_error_t cwist_job_queue_nack(cwist_job_queue_t *q, cwist_job_t *job) {
    if (!q || !job || job->q != q) return jq_err(CWIST_ERROR_INVALID_PARAM);
    cwist_error_t err = q->backend == 0 ? jq_redis_nack(q, job) : jq_nats_nack(q, job);
    cwist_job_free(job);
    return err;
}

cwist_error_t cwist_job_queue_dead_count(cwist_job_queue_t *q, uint64_t *out_count) {
    if (!q || !out_count) return jq_err(CWIST_ERROR_INVALID_PARAM);
    if (!q->dead_count) return jq_err(CWIST_FAILURE);
    return q->dead_count(q, out_count);
}
