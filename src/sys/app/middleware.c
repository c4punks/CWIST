#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include "../../net/http/http_date_internal.h"
#include <cwist/core/macros.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/siphash/siphash.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include "../../net/http/async_internal.h"
#include "log_async.h"

/**
 * @file middleware.c
 * @brief Built-in middleware implementations for request IDs, logging, rate limits, CORS, and JWT
 * auth.
 */

static pthread_mutex_t rid_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t rid_key[16];
static uint64_t rid_counter = 0;
static bool rid_keyed = false;
#ifndef __wasi__
static pid_t rid_pid = 0;
#endif

/**
 * @brief Generate a short pseudo-random request identifier for logging and tracing.
 * @param id Receives 16 identifier characters and a NUL terminator.
 *
 * Writes into the caller's buffer so the middleware has no allocation that
 * can fail on a request path (#341). Each process keys SipHash from the
 * entropy source (re-keyed after fork) and hashes a per-process counter.
 * The old time(NULL) ^ pthread_self() seed was identical in forked workers
 * that took their first request in the same second, so they emitted the
 * same ID sequence.
 */
static void generate_request_id(char id[17]) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";

    pthread_mutex_lock(&rid_mutex);
#ifndef __wasi__
    pid_t pid = getpid();
    if (!rid_keyed || rid_pid != pid) {
        cwist_generate_hash_seed(rid_key);
        rid_pid = pid;
        rid_keyed = true;
    }
#else
    if (!rid_keyed) {
        cwist_generate_hash_seed(rid_key);
        rid_keyed = true;
    }
#endif
    uint64_t n = rid_counter++;
    uint8_t key[16];
    memcpy(key, rid_key, sizeof(key));
    pthread_mutex_unlock(&rid_mutex);

    for (int half = 0; half < 2; half++) {
        uint64_t in = (n << 1) | (uint64_t)half;
        uint64_t v = siphash24(&in, sizeof(in), key);
        for (int i = 0; i < 8; i++) {
            id[half * 8 + i] = charset[v % (sizeof(charset) - 1)];
            v /= (sizeof(charset) - 1);
        }
    }
    id[16] = '\0';
}

static bool request_id_is_valid(const char *rid) {
    if (!rid || !*rid) return false;
    size_t len = strlen(rid);
    if (len > 128) return false;
    for (const unsigned char *p = (const unsigned char *)rid; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') return false;
    }
    return true;
}

/**
 * @brief Attach an X-Request-Id header to both the request and the response.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_request_id_handler(cwist_http_request *req, cwist_http_response *res,
                                 cwist_handler_func next) {
    const char *header_name = "X-Request-Id";
    const char *rid = cwist_http_header_get(req->headers, header_name);
    char generated[17];

    if (!request_id_is_valid(rid)) {
        generate_request_id(generated);
        rid = generated;
        cwist_http_header_add(&req->headers, header_name, rid);
    }

    /* header_add copies the value, so neither the request's own header nor
     * the stack buffer has to outlive this call. */
    cwist_http_header_add(&res->headers, header_name, rid);

    next(req, res);
}

/**
 * @brief Return the built-in request ID middleware.
 * @param header_name NULL or "X-Request-Id"; other names are unsupported.
 * @return Middleware function pointer, or NULL for an unsupported header name.
 */
cwist_middleware_func cwist_mw_request_id(const char *header_name) {
    if (header_name && strcasecmp(header_name, "X-Request-Id") != 0) return NULL;
    return cwist_mw_request_id_handler;
}

/* --- Access Log Middleware --- */

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Write one access-log line to stdout.
 * The line is formatted once and queued for the async log writer when
 * cwist_log_async_start() is active; otherwise it goes to stdout under
 * log_mutex as before.
 * @param fmt printf-style format of the whole line, newline included.
 */
static void access_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void access_log_printf(const char *fmt, ...) {
    char stack_buf[1024];
    char *line = stack_buf;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n >= sizeof(stack_buf)) {
        line = cwist_alloc((size_t)n + 1);
        if (!line) return;
        va_start(args, fmt);
        vsnprintf(line, (size_t)n + 1, fmt, args);
        va_end(args);
    }
    if (!cwist_log_async_submit(STDOUT_FILENO, line, (size_t)n)) {
        pthread_mutex_lock(&log_mutex);
        fwrite(line, 1, (size_t)n, stdout);
        pthread_mutex_unlock(&log_mutex);
    }
    if (line != stack_buf) cwist_free(line);
}

static char *access_log_escape(const char *input, bool json) {
    if (!input) return cwist_strdup("-");
    size_t len = strlen(input);
    if (len > (SIZE_MAX - 1) / 6) return NULL;
    char *escaped = cwist_alloc(len * 6 + 1);
    if (!escaped) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '"' || c == '\\') {
            escaped[out++] = '\\';
            escaped[out++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            if (json) {
                out += (size_t)snprintf(escaped + out, 7, "\\u%04x", c);
            } else {
                out += (size_t)snprintf(escaped + out, 5, "\\x%02x", c);
            }
        } else {
            escaped[out++] = (char)c;
        }
    }
    escaped[out] = '\0';
    return escaped;
}

/**
 * @brief Format a timestamp in Common Log Format: dd/Mon/yyyy:HH:MM:SS +zzzz
 *        in local time.
 * @param buf Output buffer.
 * @param len Size of @p buf in bytes.
 * @param tv Timestamp to format.
 */
static void format_clf_time(char *buf, size_t len, const struct timeval *tv) {
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    char tzbuf[8];
    strftime(tzbuf, sizeof(tzbuf), "%z", &tm);
    int mon = tm.tm_mon >= 0 && tm.tm_mon < 12 ? tm.tm_mon : 0;
    snprintf(buf, len, "%02d/%s/%04d:%02d:%02d:%02d %s", tm.tm_mday, cwist_http_month_names[mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec, tzbuf);
}

/**
 * @brief Format a timestamp in UTC ISO 8601 with millisecond precision:
 *        yyyy-MM-ddTHH:MM:SS.mmmZ.
 * @param buf Output buffer.
 * @param len Size of @p buf in bytes.
 * @param tv Timestamp to format.
 */
static void format_iso8601_time(char *buf, size_t len, const struct timeval *tv) {
    struct tm tm;
    gmtime_r(&tv->tv_sec, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
    size_t pos = strlen(buf);
    snprintf(buf + pos, len - pos, ".%03ldZ", tv->tv_usec / 1000);
}

/**
 * @brief Resolve the request ID to log, preferring the response headers.
 *
 * After next() a deferred response belongs to its cwist_async completion,
 * which may be writing it on another thread right now (cwist_async_respond(),
 * the timeout job). The access logs then do not read it: status and size are
 * logged as unknown and the request id comes from the request,
 * where cwist_mw_request_id() also puts it.
 *
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @return Header value of X-Request-Id, or NULL if absent.
 */
static const char *access_log_request_id(cwist_http_request *req, cwist_http_response *res) {
    cwist_http_header_node *headers = cwist_async_res_deferred(res) ? req->headers : res->headers;
    const char *rid = cwist_http_header_get(headers, "X-Request-Id");
    if (!rid && headers != req->headers) rid = cwist_http_header_get(req->headers, "X-Request-Id");
    return rid;
}

/**
 * @brief Render response status code and body size as strings for access logs.
 * @param res Outgoing HTTP response.
 * @param unknown Placeholder written to both outputs when @p res is deferred
 *        and its status/size cannot be read safely.
 * @param status Output buffer for the status code.
 * @param status_len Size of @p status in bytes.
 * @param bytes Output buffer for the response body size.
 * @param bytes_len Size of @p bytes in bytes.
 */
static void access_log_status_bytes(const cwist_http_response *res, const char *unknown,
                                    char *status, size_t status_len, char *bytes,
                                    size_t bytes_len) {
    if (cwist_async_res_deferred(res)) {
        snprintf(status, status_len, "%s", unknown);
        snprintf(bytes, bytes_len, "%s", unknown);
        return;
    }
    snprintf(status, status_len, "%d", (int)res->status_code);
    snprintf(bytes, bytes_len, "%zu", res->body ? res->body->size : (size_t)0);
}

/**
 * @brief Common Log Format access-log middleware; prints one line per request
 *        to stdout after the handler completes.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
static void cwist_mw_access_log_common_handler(cwist_http_request *req, cwist_http_response *res,
                                               cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);

    const char *rid = access_log_request_id(req, res);
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_clf_time(time_buf, sizeof(time_buf), &end);

    char status[16], res_bytes[32];
    access_log_status_bytes(res, "-", status, sizeof(status), res_bytes, sizeof(res_bytes));

    char *rid_text = access_log_escape(rid, false);
    char *path = access_log_escape(req->path ? req->path->data : "-", false);
    char *query = access_log_escape(req->query ? req->query->data : "", false);
    if (rid_text && path && query) {
        access_log_printf("%s - %s [%s] \"%s %s%s%s %s\" %s %s\n", ip_str, rid_text, time_buf,
                          cwist_http_method_to_string(req->method), path,
                          query[0] ? "?" : "", query,
                          req->version ? req->version->data : "HTTP/1.1", status, res_bytes);
    }
    cwist_free(rid_text);
    cwist_free(path);
    cwist_free(query);

    if (ip) cwist_sstring_destroy(ip);
}

/**
 * @brief Combined Log Format access-log middleware; like the common format but
 *        also logs Referer and User-Agent, one line per request to stdout.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
static void cwist_mw_access_log_combined_handler(cwist_http_request *req, cwist_http_response *res,
                                                 cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);

    const char *rid = access_log_request_id(req, res);
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_clf_time(time_buf, sizeof(time_buf), &end);

    const char *referer = cwist_http_header_get(req->headers, "Referer");
    const char *user_agent = cwist_http_header_get(req->headers, "User-Agent");
    char status[16], res_bytes[32];
    access_log_status_bytes(res, "-", status, sizeof(status), res_bytes, sizeof(res_bytes));

    char *rid_text = access_log_escape(rid, false);
    char *path = access_log_escape(req->path ? req->path->data : "-", false);
    char *query = access_log_escape(req->query ? req->query->data : "", false);
    char *referer_text = access_log_escape(referer, false);
    char *agent_text = access_log_escape(user_agent, false);
    if (rid_text && path && query && referer_text && agent_text) {
        access_log_printf("%s - %s [%s] \"%s %s%s%s %s\" %s %s \"%s\" \"%s\"\n", ip_str, rid_text,
                          time_buf, cwist_http_method_to_string(req->method), path,
                          query[0] ? "?" : "", query,
                          req->version ? req->version->data : "HTTP/1.1", status, res_bytes,
                          referer_text, agent_text);
    }
    cwist_free(rid_text);
    cwist_free(path);
    cwist_free(query);
    cwist_free(referer_text);
    cwist_free(agent_text);

    if (ip) cwist_sstring_destroy(ip);
}

/**
 * @brief JSON access-log middleware; prints one JSON object per request to
 *        stdout, including status, duration, and request/response byte counts.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
static void cwist_mw_access_log_json_handler(cwist_http_request *req, cwist_http_response *res,
                                             cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);
    long msec = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;

    const char *rid = access_log_request_id(req, res);
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_iso8601_time(time_buf, sizeof(time_buf), &end);

    size_t req_bytes = req->body ? req->body->size : 0;
    char status[16], res_bytes[32];
    access_log_status_bytes(res, "null", status, sizeof(status), res_bytes, sizeof(res_bytes));

    char *rid_text = access_log_escape(rid, true);
    char *path = access_log_escape(req->path ? req->path->data : "-", true);
    char *query = access_log_escape(req->query ? req->query->data : "", true);
    if (rid_text && path && query) {
        access_log_printf(
            "{\"time\":\"%s\",\"client\":\"%s\",\"rid\":\"%s\",\"method\":\"%s\",\"path\":\"%s%s%s\",\"protocol\":\"%s\",\"status\":%s,\"duration_ms\":%ld,\"req_bytes\":%zu,\"res_bytes\":%s}\n",
            time_buf, ip_str, rid_text, cwist_http_method_to_string(req->method), path,
            query[0] ? "?" : "", query, req->version ? req->version->data : "HTTP/1.1", status,
            msec, req_bytes, res_bytes);
    }
    cwist_free(rid_text);
    cwist_free(path);
    cwist_free(query);

    if (ip) cwist_sstring_destroy(ip);
}

/**
 * @brief Return the built-in access-log middleware.
 * @param format Log format selector (COMMON, COMBINED, JSON).
 * @return Middleware function pointer for access logging.
 */
cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format) {
    switch (format) {
        case CWIST_LOG_COMMON: return cwist_mw_access_log_common_handler;
        case CWIST_LOG_COMBINED: return cwist_mw_access_log_combined_handler;
        case CWIST_LOG_JSON: return cwist_mw_access_log_json_handler;
        default: return cwist_mw_access_log_common_handler;
    }
}

/* --- Metrics Middleware --- */

#include <cwist/sys/metrics/metrics.h>

/**
 * @brief Increment request counter and observe duration for Prometheus metrics.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_metrics_handler(cwist_http_request *req, cwist_http_response *res,
                              cwist_handler_func next) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    next(req, res);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long duration_ns = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    if (reg) {
        cwist_metric_inc(reg, CWIST_METRIC_REQUESTS_TOTAL);
        cwist_metric_add(reg, CWIST_METRIC_REQUEST_DURATION_NS, (uintmax_t)duration_ns);
    }
}

/**
 * @brief Return the built-in metrics collection middleware.
 * @return Middleware function pointer for request metrics.
 */
cwist_middleware_func cwist_mw_metrics(void) {
    return cwist_mw_metrics_handler;
}

/* --- Rate Limiter Middleware --- */

#include <ttak/limit/limit.h>
#include <ttak/timing/timing.h>
#include <arpa/inet.h>

#define IP_BUCKET_IDLE_MS 600000

typedef struct {
    char ip[46];
    int slot;
    uint64_t last_used_ms;
    ttak_token_bucket_t bucket;
    bool active;
} ip_bucket_t;

#define MAX_IP_BUCKETS 1024
static ip_bucket_t ip_buckets[MAX_IP_BUCKETS];
static int ip_bucket_count = 0;
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rate_limit_ip_key(const char *input, char output[46]) {
    struct in6_addr addr;
    if (input && inet_pton(AF_INET6, input, &addr) == 1) {
        memset(addr.s6_addr + 8, 0, 8);
        if (inet_ntop(AF_INET6, &addr, output, 46)) return;
    }
    snprintf(output, 46, "%s", input ? input : "");
}

/**
 * @brief Per-IP token-bucket rate-limit handler; rejects with 429 when the
 *        client exceeds the allowed requests per minute.
 *
 * The rate is passed in, not through req->private_data: inside the app's
 * middleware chain that field holds the chain's own state, which next()
 * reads.
 *
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 * @param slot Limiter configuration instance.
 * @param rpm Allowed requests per minute (token bucket refill rate).
 */
static void cwist_mw_rate_limit_ip_handler(cwist_http_request *req, cwist_http_response *res,
                                           cwist_handler_func next, int slot, int rpm) {
    if (rpm <= 0) rpm = 60;

    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    if (!ip) {
        next(req, res);
        return;
    }
    char key[46];
    rate_limit_ip_key(ip->data, key);

    uint64_t now = ttak_get_tick_count();
    pthread_mutex_lock(&rate_mutex);
    ip_bucket_t *found = NULL, *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < ip_bucket_count; i++) {
        ip_bucket_t *entry = &ip_buckets[i];
        if (!entry->active) {
            if (!free_slot) free_slot = entry;
            continue;
        }
        if (now - entry->last_used_ms > IP_BUCKET_IDLE_MS) {
            entry->active = false;
            if (!free_slot) free_slot = entry;
            continue;
        }
        if (!oldest || entry->last_used_ms < oldest->last_used_ms) oldest = entry;
        if (entry->slot == slot && strcmp(entry->ip, key) == 0) {
            found = entry;
            break;
        }
    }

    if (!found) {
        if (free_slot)
            found = free_slot;
        else if (ip_bucket_count < MAX_IP_BUCKETS)
            found = &ip_buckets[ip_bucket_count++];
        else
            found = oldest;
        if (found) {
            snprintf(found->ip, sizeof(found->ip), "%s", key);
            found->slot = slot;
            found->active = true;
            double rate = rpm / 60.0;
            ttak_token_bucket_init(&found->bucket, rate, (double)rpm);
        }
    }
    if (found) found->last_used_ms = now;

    bool allowed = !found || ttak_token_bucket_consume(&found->bucket, 1.0);
    cwist_sstring_destroy(ip);
    pthread_mutex_unlock(&rate_mutex);

    if (!allowed) {
        res->status_code = 429;
        cwist_sstring_assign(res->body, "Too Many Requests");
        char retry_buf[16];
        snprintf(retry_buf, sizeof(retry_buf), "%d", 60 / rpm > 0 ? 60 / rpm : 1);
        cwist_http_header_add(&res->headers, "Retry-After", retry_buf);
        return;
    }

    next(req, res);
}

#define CWIST_RATE_LIMIT_MAX_CFGS 8

typedef struct {
    int rpm;
} rate_limit_cfg_t;

static rate_limit_cfg_t s_rate_cfgs[CWIST_RATE_LIMIT_MAX_CFGS];
static int s_rate_cfg_count = 0;
static pthread_mutex_t s_rate_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CWIST_RATE_LIMIT_DEFINE_WRAPPER(N)                                                      \
    static void cwist_mw_rate_limit_wrap_##N(cwist_http_request *req, cwist_http_response *res, \
                                             cwist_handler_func next) {                         \
        cwist_mw_rate_limit_ip_handler(req, res, next, N, s_rate_cfgs[N].rpm);                  \
    }

CWIST_RATE_LIMIT_DEFINE_WRAPPER(0)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(1)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(2)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(3)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(4)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(5)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(6)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(7)

static cwist_middleware_func s_rate_wrappers[CWIST_RATE_LIMIT_MAX_CFGS] = {
    cwist_mw_rate_limit_wrap_0, cwist_mw_rate_limit_wrap_1, cwist_mw_rate_limit_wrap_2,
    cwist_mw_rate_limit_wrap_3, cwist_mw_rate_limit_wrap_4, cwist_mw_rate_limit_wrap_5,
    cwist_mw_rate_limit_wrap_6, cwist_mw_rate_limit_wrap_7,
};

/**
 * @brief Reset all per-IP rate-limiter buckets, clearing tracked clients.
 */
void cwist_mw_rate_limit_reset(void) {
    pthread_mutex_lock(&rate_mutex);
    ip_bucket_count = 0;
    pthread_mutex_unlock(&rate_mutex);
}

/**
 * @brief Return an independent per-IP rate-limiter middleware instance.
 *
 * Every call allocates a distinct limiter configuration, including repeated
 * calls with the same rate. Returns NULL when all configuration slots are used.
 *
 * @param requests_per_minute Allowed requests per minute (token bucket rate);
 *        values <= 0 fall back to 60.
 * @return Middleware function pointer for per-IP rate limiting, or NULL.
 */
cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute) {
    if (requests_per_minute <= 0) requests_per_minute = 60;

    pthread_mutex_lock(&s_rate_cfg_mutex);
    if (s_rate_cfg_count >= CWIST_RATE_LIMIT_MAX_CFGS) {
        pthread_mutex_unlock(&s_rate_cfg_mutex);
        fprintf(stderr, "[CWIST] cwist_mw_rate_limit_ip: maximum config slots exceeded\n");
        return NULL;
    }

    int slot = s_rate_cfg_count++;
    s_rate_cfgs[slot].rpm = requests_per_minute;

    pthread_mutex_unlock(&s_rate_cfg_mutex);
    return s_rate_wrappers[slot];
}

/* --- CORS Middleware --- */

/**
 * @brief Inject permissive CORS headers and short-circuit preflight requests.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_cors_handler(cwist_http_request *req, cwist_http_response *res,
                           cwist_handler_func next) {
    // Add standard CORS headers
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");

    // Handle Preflight (OPTIONS)
    if (req->method == CWIST_HTTP_OPTIONS && cwist_http_header_get(req->headers, "Origin") &&
        cwist_http_header_get(req->headers, "Access-Control-Request-Method")) {
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods",
                              "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD");
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers",
                              "Content-Type, Authorization, X-Request-Id, X-CSRF-Token");
        cwist_http_header_add(&res->headers, "Access-Control-Max-Age", "86400"); // 24 hours

        res->status_code = CWIST_HTTP_NO_CONTENT; // 204 No Content
        // Short-circuit: do not call next()
        return;
    }

    next(req, res);
}

/**
 * @brief Return the built-in permissive CORS middleware.
 * @return Middleware function pointer for CORS handling.
 */
cwist_middleware_func cwist_mw_cors(void) {
    return cwist_mw_cors_handler;
}

/* --- JWT Authentication Middleware --- */

/*
 * The decoded claims of a request reach downstream handlers through a
 * per-thread stack of contexts, one per JWT middleware currently running,
 * each tagged with its request.  req->private_data is not used: inside the
 * app's middleware chain it holds the chain's own state, which next() and
 * the final route handler read.
 *
 * Each context is stack-allocated in the middleware frame and is only valid
 * while the rest of the chain runs synchronously inside next(); claims are
 * destroyed when next() returns, so no pointer to them escapes to
 * asynchronous code.
 */

typedef struct cwist_jwt_ctx {
    const cwist_http_request *req; ///< Request these claims belong to.
    cwist_jwt_claims *claims;      ///< Decoded claims; owned by the middleware frame.
    struct cwist_jwt_ctx *outer;   ///< Enclosing JWT context on this thread.
} cwist_jwt_ctx_t;

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
static _Thread_local cwist_jwt_ctx_t *t_jwt_ctx = NULL;
#elif defined(__GNUC__) || defined(__clang__)
static __thread cwist_jwt_ctx_t *t_jwt_ctx = NULL;
#endif

/**
 * @brief Validate a bearer token and expose its decoded claims to downstream handlers.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 * @param secret Signing secret of the slot this middleware was created for.
 */
static void cwist_mw_jwt_auth_handler(cwist_http_request *req, cwist_http_response *res,
                                      cwist_handler_func next, const char *secret) {
    /* Extract Bearer token from Authorization header */
    char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    if (!auth_header) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Missing Authorization header\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_http_header_add(&res->headers, "WWW-Authenticate", "Bearer realm=\"api\"");
        return;
    }

    /* Expect "Bearer <token>" (scheme is case-insensitive per RFC 6750) */
    if (strncasecmp(auth_header, "Bearer ", 7) != 0) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid Authorization scheme\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_http_header_add(&res->headers, "WWW-Authenticate",
                              "Bearer realm=\"api\", error=\"invalid_request\"");
        return;
    }

    const char *token = auth_header + 7;
    while (*token == ' ' || *token == '\t') token++;

    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    if (!claims) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid or expired token\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_http_header_add(&res->headers, "WWW-Authenticate",
                              "Bearer realm=\"api\", error=\"invalid_token\"");
        return;
    }

    /* Publish the claims to downstream handlers on this thread for the
     * duration of the rest of the chain. */
    cwist_jwt_ctx_t ctx = {.req = req, .claims = claims, .outer = t_jwt_ctx};
    t_jwt_ctx = &ctx;

    if (next) next(req, res);

    t_jwt_ctx = ctx.outer;
    cwist_jwt_claims_destroy(claims);
}

typedef struct {
    const char *secret;
} cwist_jwt_mw_cfg_t;

/* We keep a small static table of registered secrets.  A server rarely needs
 * more than a handful of distinct signing keys.  8 slots cover the common case
 * (e.g. one per audience / service) without dynamic allocation.  If more are
 * needed, increase CWIST_JWT_MAX_SECRETS and add the corresponding wrapper. */
#define CWIST_JWT_MAX_SECRETS 8

static cwist_jwt_mw_cfg_t s_jwt_cfgs[CWIST_JWT_MAX_SECRETS];
static int s_jwt_cfg_count = 0;
static pthread_mutex_t s_jwt_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* One wrapper function per registered secret slot, because
 * cwist_middleware_func is a plain function pointer that cannot carry the
 * secret itself. */
#define CWIST_JWT_DEFINE_WRAPPER(N)                                                      \
    static void cwist_mw_jwt_wrap_##N(cwist_http_request *req, cwist_http_response *res, \
                                      cwist_handler_func next) {                         \
        cwist_mw_jwt_auth_handler(req, res, next, s_jwt_cfgs[N].secret);                 \
    }

CWIST_JWT_DEFINE_WRAPPER(0)
CWIST_JWT_DEFINE_WRAPPER(1)
CWIST_JWT_DEFINE_WRAPPER(2)
CWIST_JWT_DEFINE_WRAPPER(3)
CWIST_JWT_DEFINE_WRAPPER(4)
CWIST_JWT_DEFINE_WRAPPER(5)
CWIST_JWT_DEFINE_WRAPPER(6)
CWIST_JWT_DEFINE_WRAPPER(7)

static cwist_middleware_func s_jwt_wrappers[CWIST_JWT_MAX_SECRETS] = {
    cwist_mw_jwt_wrap_0, cwist_mw_jwt_wrap_1, cwist_mw_jwt_wrap_2, cwist_mw_jwt_wrap_3,
    cwist_mw_jwt_wrap_4, cwist_mw_jwt_wrap_5, cwist_mw_jwt_wrap_6, cwist_mw_jwt_wrap_7,
};

/**
 * @brief Register a JWT secret in a static slot and return the matching middleware wrapper.
 * @param secret Borrowed signing secret used to verify bearer tokens.
 * @return Middleware wrapper bound to the supplied secret, or NULL on overflow.
 */
cwist_middleware_func cwist_mw_jwt_auth(const char *secret) {
    if (!secret) return NULL;

    pthread_mutex_lock(&s_jwt_cfg_mutex);

    /* Reuse existing slot for the same secret */
    for (int i = 0; i < s_jwt_cfg_count; i++) {
        if (s_jwt_cfgs[i].secret && strcmp(s_jwt_cfgs[i].secret, secret) == 0) {
            pthread_mutex_unlock(&s_jwt_cfg_mutex);
            return s_jwt_wrappers[i];
        }
    }

    if (s_jwt_cfg_count >= CWIST_JWT_MAX_SECRETS) {
        pthread_mutex_unlock(&s_jwt_cfg_mutex);
        fprintf(stderr, "[CWIST] cwist_mw_jwt_auth: maximum secret slots exceeded\n");
        return NULL;
    }

    int slot = s_jwt_cfg_count++;
    s_jwt_cfgs[slot].secret = secret;

    pthread_mutex_unlock(&s_jwt_cfg_mutex);
    return s_jwt_wrappers[slot];
}

/**
 * @brief Retrieve the claims the JWT middleware decoded for a request.
 * @param req Request currently executing behind the JWT middleware.
 * @return Active decoded claims, or NULL when the request is not inside JWT auth
 *         on this thread.
 */
const cwist_jwt_claims *cwist_mw_jwt_get_claims(const cwist_http_request *req) {
    if (!req) return NULL;
    for (const cwist_jwt_ctx_t *ctx = t_jwt_ctx; ctx; ctx = ctx->outer) {
        if (ctx->req == req) return ctx->claims;
    }
    return NULL;
}
