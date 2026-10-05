#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <cwist/core/macros.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>

/**
 * @file middleware.c
 * @brief Built-in middleware implementations for request IDs, logging, rate limits, CORS, and JWT
 * auth.
 */

static pthread_mutex_t rid_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int rid_seed = 0;

/**
 * @brief Generate a short pseudo-random request identifier for logging and tracing.
 * @return Heap-allocated 16-character identifier string.
 */
static char *generate_request_id() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char *id = cwist_alloc(17);

    pthread_mutex_lock(&rid_mutex);
    if (rid_seed == 0) rid_seed = (unsigned int)time(NULL) ^ (unsigned int)pthread_self();
    unsigned int seed = rid_seed++;
    pthread_mutex_unlock(&rid_mutex);

    for (int i = 0; i < 16; i++) {
        id[i] = charset[rand_r(&seed) % (sizeof(charset) - 1)];
    }
    id[16] = '\0';
    return id;
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
    char *existing = cwist_http_header_get(req->headers, header_name);
    char *rid;

    if (existing) {
        rid = cwist_strdup(existing);
    } else {
        rid = generate_request_id();
        cwist_http_header_add(&req->headers, header_name, rid);
    }

    cwist_http_header_add(&res->headers, header_name, rid);

    next(req, res);
    cwist_free(rid);
}

/**
 * @brief Return the built-in request ID middleware.
 * @param header_name Currently unused custom header override.
 * @return Middleware function pointer for request ID injection.
 */
cwist_middleware_func cwist_mw_request_id(const char *header_name) {
    CWIST_UNUSED(header_name);
    return cwist_mw_request_id_handler;
}

/* --- Access Log Middleware --- */

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    strftime(buf, len, "%d/%b/%Y:%H:%M:%S", &tm);
    size_t pos = strlen(buf);
    snprintf(buf + pos, len - pos, " %s", tzbuf);
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
    cwist_http_header_node *headers = res->deferred ? req->headers : res->headers;
    return cwist_http_header_get(headers, "X-Request-Id");
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
    if (res->deferred) {
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

    pthread_mutex_lock(&log_mutex);
    printf("%s - %s [%s] \"%s %s %s\" %s %s\n", ip_str, rid ? rid : "-", time_buf,
           cwist_http_method_to_string(req->method), req->path->data,
           req->version ? req->version->data : "HTTP/1.1", status, res_bytes);
    pthread_mutex_unlock(&log_mutex);

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

    pthread_mutex_lock(&log_mutex);
    printf("%s - %s [%s] \"%s %s %s\" %s %s \"%s\" \"%s\"\n", ip_str, rid ? rid : "-", time_buf,
           cwist_http_method_to_string(req->method), req->path->data,
           req->version ? req->version->data : "HTTP/1.1", status, res_bytes,
           referer ? referer : "-", user_agent ? user_agent : "-");
    pthread_mutex_unlock(&log_mutex);

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

    pthread_mutex_lock(&log_mutex);
    printf(
        "{\"time\":\"%s\",\"client\":\"%s\",\"rid\":\"%s\",\"method\":\"%s\",\"path\":\"%s\",\"protocol\":\"%s\",\"status\":%s,\"duration_ms\":%ld,\"req_bytes\":%zu,\"res_bytes\":%s}\n",
        time_buf, ip_str, rid ? rid : "-", cwist_http_method_to_string(req->method),
        req->path->data, req->version ? req->version->data : "HTTP/1.1", status, msec, req_bytes,
        res_bytes);
    pthread_mutex_unlock(&log_mutex);

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

typedef struct {
    char ip[46];
    ttak_token_bucket_t bucket;
    bool active;
} ip_bucket_t;

#define MAX_IP_BUCKETS 1024
static ip_bucket_t ip_buckets[MAX_IP_BUCKETS];
static int ip_bucket_count = 0;
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;

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
 * @param rpm Allowed requests per minute (token bucket refill rate).
 */
static void cwist_mw_rate_limit_ip_handler(cwist_http_request *req, cwist_http_response *res,
                                           cwist_handler_func next, int rpm) {
    if (rpm <= 0) rpm = 60;

    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    if (!ip) {
        next(req, res);
        return;
    }

    pthread_mutex_lock(&rate_mutex);
    ip_bucket_t *found = NULL;
    for (int i = 0; i < ip_bucket_count; i++) {
        if (ip_buckets[i].active && strcmp(ip_buckets[i].ip, ip->data) == 0) {
            found = &ip_buckets[i];
            break;
        }
    }

    if (!found && ip_bucket_count < MAX_IP_BUCKETS) {
        found = &ip_buckets[ip_bucket_count++];
        strncpy(found->ip, ip->data, sizeof(found->ip) - 1);
        found->ip[sizeof(found->ip) - 1] = '\0';
        found->active = true;
        double rate = rpm / 60.0;
        ttak_token_bucket_init(&found->bucket, rate, (double)rpm);
    }

    /* Fail-open: if the IP table is full we cannot track this client, so
     * allow the request rather than permanently blocking unknown IPs (which
     * would let an attacker exhaust the 1024 slots with spoofed addresses
     * and then deny service to all legitimate users). */
    bool allowed = (found == NULL);
    if (found) {
        allowed = ttak_token_bucket_consume(&found->bucket, 1.0);
    }
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
        cwist_mw_rate_limit_ip_handler(req, res, next, s_rate_cfgs[N].rpm);                     \
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
 * @brief Return the built-in per-IP rate-limiter middleware.
 *
 * Reuses an existing wrapper when the same rate was already registered.  If
 * all CWIST_RATE_LIMIT_MAX_CFGS config slots are taken, logs a warning and
 * falls back to the first registered wrapper.
 *
 * @param requests_per_minute Allowed requests per minute (token bucket rate);
 *        values <= 0 fall back to 60.
 * @return Middleware function pointer for per-IP rate limiting.
 */
cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute) {
    if (requests_per_minute <= 0) requests_per_minute = 60;

    pthread_mutex_lock(&s_rate_cfg_mutex);

    for (int i = 0; i < s_rate_cfg_count; i++) {
        if (s_rate_cfgs[i].rpm == requests_per_minute) {
            pthread_mutex_unlock(&s_rate_cfg_mutex);
            return s_rate_wrappers[i];
        }
    }

    if (s_rate_cfg_count >= CWIST_RATE_LIMIT_MAX_CFGS) {
        pthread_mutex_unlock(&s_rate_cfg_mutex);
        fprintf(stderr, "[CWIST] cwist_mw_rate_limit_ip: maximum config slots exceeded\n");
        return s_rate_wrappers[0];
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
    if (req->method == CWIST_HTTP_OPTIONS) {
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods",
                              "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD");
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers",
                              "Content-Type, Authorization, X-Request-Id");
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
        return;
    }

    /* Expect "Bearer <token>" */
    if (strncmp(auth_header, "Bearer ", 7) != 0) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid Authorization scheme\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        return;
    }

    const char *token = auth_header + 7;

    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    if (!claims) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid or expired token\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
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

    /* Reuse existing slot for the same secret pointer */
    for (int i = 0; i < s_jwt_cfg_count; i++) {
        if (s_jwt_cfgs[i].secret == secret) {
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
