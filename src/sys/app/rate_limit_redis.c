/** @file rate_limit_redis.c
 * @brief Optional process-shared Redis rate limiter.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define REDIS_RATE_SLOTS 8
#define REDIS_RATE_PREFIX_MAX 128

typedef struct {
    cwist_redis_pool_t *pool;
    int rpm;
    unsigned timeout_ms;
    bool fail_closed;
    char prefix[REDIS_RATE_PREFIX_MAX + 1];
} redis_rate_cfg;

static redis_rate_cfg rate_cfgs[REDIS_RATE_SLOTS];
static size_t rate_cfg_count;
static atomic_long rate_warning_pids[REDIS_RATE_SLOTS];
static pthread_mutex_t rate_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* A server-side window avoids process clock skew. TTL is set in the same
 * atomic operation as INCR and is not extended by later or denied requests.
 * Repair a pre-existing counter without an expiry instead of leaking it. */
static const char rate_script[] = "local n=redis.call('INCR',KEYS[1]);"
                                  "if n==1 or redis.call('PTTL',KEYS[1])<0 then "
                                  "redis.call('PEXPIRE',KEYS[1],60000);end;return n";

static bool peer_ip(int fd, char ip[INET6_ADDRSTRLEN]) {
    struct sockaddr_storage peer = {0};
    socklen_t size = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &size) != 0) return false;
    if (peer.ss_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
        return inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, ip, INET6_ADDRSTRLEN) !=
               NULL;
    }
    if (peer.ss_family == AF_INET6 && size >= sizeof(struct sockaddr_in6)) {
        struct in6_addr *addr = &((struct sockaddr_in6 *)&peer)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(addr)) {
            return inet_ntop(AF_INET, (const unsigned char *)addr + 12, ip, INET6_ADDRSTRLEN) !=
                   NULL;
        }
        return inet_ntop(AF_INET6, addr, ip, INET6_ADDRSTRLEN) != NULL;
    }
    return false;
}

static void backend_failure(const redis_rate_cfg *cfg, cwist_http_request *req,
                            cwist_http_response *res, cwist_handler_func next, int code) {
    size_t slot = (size_t)(cfg - rate_cfgs);
    long pid = (long)getpid();
    if (atomic_exchange_explicit(&rate_warning_pids[slot], pid, memory_order_relaxed) != pid) {
        fprintf(stderr,
                "[CWIST] Redis rate limiter backend error %d (%s); "
                "check pool process ownership and Redis connectivity/permissions.\n",
                code, cfg->fail_closed ? "fail-closed" : "fail-open");
    }
    if (!cfg->fail_closed) {
        next(req, res);
        return;
    }
    res->status_code = 503;
    cwist_sstring_assign(res->body, "Rate limit backend unavailable");
}

static void redis_rate_handler(size_t slot, cwist_http_request *req, cwist_http_response *res,
                               cwist_handler_func next) {
    const redis_rate_cfg *cfg = &rate_cfgs[slot];
    char ip[INET6_ADDRSTRLEN];
    if (!peer_ip(req->client_fd, ip)) {
        backend_failure(cfg, req, res, next, -ENOTCONN);
        return;
    }
    char key[REDIS_RATE_PREFIX_MAX + 1 + INET6_ADDRSTRLEN];
    snprintf(key, sizeof(key), "%s%s", cfg->prefix, ip);
    const void *args[] = {"EVAL", rate_script, "1", key};
    size_t lengths[] = {4, sizeof(rate_script) - 1, 1, strlen(key)};
    cwist_redis_reply_t *reply = NULL;
    cwist_error_t err = cwist_redis_pool_command_argv_reply_timeout(cfg->pool, 4, args, lengths,
                                                                    cfg->timeout_ms, &reply);
    if (!cwist_error_is_ok(&err) || !reply || reply->type != ':' || reply->integer <= 0) {
        int code = cwist_error_is_ok(&err) ? -EPROTO : err.error.err_i16;
        cwist_error_dispose(&err);
        cwist_redis_reply_free(reply);
        backend_failure(cfg, req, res, next, code);
        return;
    }
    bool allowed = reply->integer <= cfg->rpm;
    cwist_redis_reply_free(reply);
    if (allowed) {
        next(req, res);
        return;
    }
    res->status_code = 429;
    cwist_sstring_assign(res->body, "Too Many Requests");
    cwist_http_header_add(&res->headers, "Retry-After", "60");
}

#define REDIS_RATE_WRAPPER(N)                                                          \
    static void redis_rate_wrap_##N(cwist_http_request *req, cwist_http_response *res, \
                                    cwist_handler_func next) {                         \
        redis_rate_handler(N, req, res, next);                                         \
    }
REDIS_RATE_WRAPPER(0)
REDIS_RATE_WRAPPER(1)
REDIS_RATE_WRAPPER(2)
REDIS_RATE_WRAPPER(3)
REDIS_RATE_WRAPPER(4)
REDIS_RATE_WRAPPER(5)
REDIS_RATE_WRAPPER(6)
REDIS_RATE_WRAPPER(7)

static cwist_middleware_func wrappers[REDIS_RATE_SLOTS] = {
    redis_rate_wrap_0, redis_rate_wrap_1, redis_rate_wrap_2, redis_rate_wrap_3,
    redis_rate_wrap_4, redis_rate_wrap_5, redis_rate_wrap_6, redis_rate_wrap_7,
};

cwist_middleware_func cwist_mw_rate_limit_ip_redis(const cwist_rate_limit_redis_cfg_t *cfg) {
    if (!cfg || !cfg->pool || cfg->requests_per_minute <= 0 || cfg->timeout_ms > 60000) return NULL;
    const char *prefix = cfg->key_prefix ? cfg->key_prefix : "cwist:rl:";
    if (strnlen(prefix, REDIS_RATE_PREFIX_MAX + 1) > REDIS_RATE_PREFIX_MAX) return NULL;
    unsigned timeout = cfg->timeout_ms ? cfg->timeout_ms : 50;
    pthread_mutex_lock(&rate_cfg_mutex);
    for (size_t i = 0; i < rate_cfg_count; ++i) {
        const redis_rate_cfg *old = &rate_cfgs[i];
        if (old->pool == cfg->pool && old->rpm == cfg->requests_per_minute &&
            old->timeout_ms == timeout && old->fail_closed == cfg->fail_closed &&
            strcmp(old->prefix, prefix) == 0) {
            pthread_mutex_unlock(&rate_cfg_mutex);
            return wrappers[i];
        }
    }
    if (rate_cfg_count == REDIS_RATE_SLOTS) {
        pthread_mutex_unlock(&rate_cfg_mutex);
        return NULL;
    }
    size_t slot = rate_cfg_count++;
    rate_cfgs[slot] = (redis_rate_cfg){.pool = cfg->pool,
                                       .rpm = cfg->requests_per_minute,
                                       .timeout_ms = timeout,
                                       .fail_closed = cfg->fail_closed};
    memcpy(rate_cfgs[slot].prefix, prefix, strlen(prefix) + 1);
    pthread_mutex_unlock(&rate_cfg_mutex);
    return wrappers[slot];
}
