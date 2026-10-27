/** Integration contract for the Redis-backed limiter. */
#ifdef NDEBUG
#error "Redis limiter tests require assertions"
#endif
#include <cwist/sys/app/middleware.h>
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void next_ok(cwist_http_request *req, cwist_http_response *res) {
    assert(req->private_data == (void *)0x403);
    res->status_code = CWIST_HTTP_OK;
}

static int peer_fd(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0 && client >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    assert(listen(listener, 1) == 0);
    assert(connect(client, (struct sockaddr *)&addr, len) == 0);
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    close(client);
    close(listener);
    return fd;
}

static int peer_fd6(void) {
    int listener = socket(AF_INET6, SOCK_STREAM, 0);
    int client = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 address = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    assert(listener >= 0 && client >= 0);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t len = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &len) == 0);
    assert(listen(listener, 1) == 0);
    assert(connect(client, (struct sockaddr *)&address, len) == 0);
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    close(client);
    close(listener);
    return fd;
}

static int request(cwist_middleware_func mw, int fd) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    assert(req && res);
    req->client_fd = fd;
    req->private_data = (void *)0x403;
    mw(req, res, next_ok);
    int status = res->status_code;
    if (status == 429) {
        assert(strcmp(cwist_http_header_get(res->headers, "Retry-After"), "60") == 0);
    }
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    return status;
}

static char *command(cwist_redis_t *redis, size_t n, const char **args) {
    size_t lengths[8];
    assert(n <= 8);
    for (size_t i = 0; i < n; ++i) lengths[i] = strlen(args[i]);
    char *out = NULL;
    cwist_error_t err =
        cwist_redis_command_argv(redis, n, (const void *const *)args, lengths, &out, NULL);
    assert(cwist_error_is_ok(&err));
    return out;
}

static long long number(cwist_redis_t *redis, const char *op, const char *key) {
    const char *args[] = {op, key};
    char *out = command(redis, 2, args);
    assert(out);
    char *end;
    long long n = strtoll(out, &end, 10);
    assert(*out && !*end);
    cwist_free(out);
    return n;
}

static void transfer(int fd, void *buf, size_t size, int writing) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = writing ? write(fd, (char *)buf + done, size - done)
                            : read(fd, (char *)buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        done += (size_t)n;
    }
}

static cwist_middleware_func limiter_budget(cwist_redis_pool_t *pool, const char *prefix, int rpm,
                                            int fail_closed, unsigned timeout_ms) {
    cwist_rate_limit_redis_cfg_t cfg = {.pool = pool,
                                        .requests_per_minute = rpm,
                                        .key_prefix = prefix,
                                        .timeout_ms = timeout_ms,
                                        .fail_closed = fail_closed};
    cwist_middleware_func mw = cwist_mw_rate_limit_ip_redis(&cfg);
    assert(mw);
    return mw;
}

static cwist_middleware_func limiter(cwist_redis_pool_t *pool, const char *prefix, int rpm,
                                     int fail_closed) {
    return limiter_budget(pool, prefix, rpm, fail_closed, 500);
}

static double now_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void test_shared_processes(int port, const char *prefix) {
    int start[2][2], result[2][2];
    pid_t children[2];
    for (int i = 0; i < 2; ++i) {
        assert(pipe(start[i]) == 0 && pipe(result[i]) == 0);
        children[i] = fork();
        assert(children[i] >= 0);
        if (children[i] == 0) {
            close(start[i][1]);
            close(result[i][0]);
            cwist_redis_pool_t *pool = cwist_redis_pool_create("127.0.0.1", port, 1);
            assert(pool);
            cwist_middleware_func mw = limiter(pool, prefix, 7, 1);
            int fd = peer_fd();
            char ready = 1;
            transfer(result[i][1], &ready, 1, 1);
            transfer(start[i][0], &ready, 1, 0);
            int counts[2] = {0};
            for (int j = 0; j < 20; ++j) {
                int status = request(mw, fd);
                assert(status == 200 || status == 429);
                ++counts[status == 429];
            }
            transfer(result[i][1], counts, sizeof(counts), 1);
            close(fd);
            cwist_redis_pool_destroy(pool);
            _exit(0);
        }
        close(start[i][0]);
        close(result[i][1]);
    }
    char ready;
    for (int i = 0; i < 2; ++i) transfer(result[i][0], &ready, 1, 0);
    for (int i = 0; i < 2; ++i) transfer(start[i][1], &ready, 1, 1);
    int total[2] = {0};
    for (int i = 0; i < 2; ++i) {
        int counts[2], status;
        transfer(result[i][0], counts, sizeof(counts), 0);
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        total[0] += counts[0];
        total[1] += counts[1];
        close(start[i][1]);
        close(result[i][0]);
    }
    assert(total[0] == 7 && total[1] == 33);
    printf("[PASS] two processes share exactly 7 admits and 33 rejections\n");
}

static void test_inherited_warning(cwist_middleware_func mw, int fd) {
    int capture[2];
    assert(pipe(capture) == 0);
    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(capture[0]);
        assert(dup2(capture[1], STDERR_FILENO) >= 0);
        close(capture[1]);
        for (int i = 0; i < 3; ++i) assert(request(mw, fd) == 200);
        fflush(stderr);
        _exit(0);
    }
    close(capture[1]);
    char text[2048] = {0};
    size_t used = 0;
    ssize_t n;
    while ((n = read(capture[0], text + used, sizeof(text) - 1 - used)) > 0) used += (size_t)n;
    assert(n == 0);
    close(capture[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char *warning = strstr(text, "Redis rate limiter backend error");
    assert(warning && strstr(warning + 1, "Redis rate limiter backend error") == NULL);
    assert(strstr(text, "fail-open"));
    puts("[PASS] inherited pool warns once in each process");
}

int main(void) {
    assert(cwist_mw_rate_limit_ip_redis(NULL) == NULL);
    cwist_rate_limit_redis_cfg_t invalid = {0};
    assert(cwist_mw_rate_limit_ip_redis(&invalid) == NULL);
    invalid.pool = (cwist_redis_pool_t *)1; /* Validation must not dereference it. */
    assert(cwist_mw_rate_limit_ip_redis(&invalid) == NULL);
    invalid.requests_per_minute = 1;
    invalid.timeout_ms = 60001;
    assert(cwist_mw_rate_limit_ip_redis(&invalid) == NULL);
    invalid.timeout_ms = 0;
    char too_long[130];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    invalid.key_prefix = too_long;
    assert(cwist_mw_rate_limit_ip_redis(&invalid) == NULL);
    const char *value = getenv("CWIST_REDIS_RATE_LIMIT_TEST_PORT");
    if (!value) {
        assert(!getenv("CWIST_REDIS_RATE_LIMIT_REQUIRE"));
        puts("[SKIP] real Redis checks need the owned-Redis runner");
        return 0;
    }
    int port = atoi(value);
    assert(port > 0);
    char prefix[96], key[128];
    snprintf(prefix, sizeof(prefix), "cwist:test:rl:%ld:", (long)getpid());
    snprintf(key, sizeof(key), "%s127.0.0.1", prefix);
    test_shared_processes(port, prefix);
    cwist_redis_t *redis = cwist_redis_connect("127.0.0.1", port);
    cwist_redis_pool_t *pool = cwist_redis_pool_create("127.0.0.1", port, 1);
    assert(redis && pool);
    assert(number(redis, "GET", key) == 40);
    assert(number(redis, "PTTL", key) > 0 && number(redis, "PTTL", key) <= 60000);
    int fd = peer_fd();
    cwist_middleware_func shared = limiter(pool, prefix, 7, 1);
    const char *expire[] = {"PEXPIRE", key, "1000"};
    cwist_free(command(redis, 3, expire));
    assert(request(shared, fd) == 429);
    assert(number(redis, "PTTL", key) <= 1000);
    puts("[PASS] rejected requests do not extend the Redis TTL");
    const char *persist[] = {"PERSIST", key};
    cwist_free(command(redis, 2, persist));
    assert(number(redis, "PTTL", key) == -1);
    assert(request(shared, fd) == 429);
    assert(number(redis, "PTTL", key) > 0);
    puts("[PASS] missing expiry is repaired atomically");
    int ipv6 = peer_fd6();
    assert(request(shared, ipv6) == 200);
    close(ipv6);
    puts("[PASS] distinct IPv6 peer has an independent bucket");
    const char *expire_now[] = {"PEXPIRE", key, "1"};
    cwist_free(command(redis, 3, expire_now));
    for (int i = 0; number(redis, "PTTL", key) != -2; ++i) {
        assert(i < 100);
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    assert(request(shared, fd) == 200);
    assert(number(redis, "GET", key) == 1);
    puts("[PASS] key expiry opens a fresh shared window");
    char other[sizeof(prefix) + sizeof("other:")];
    snprintf(other, sizeof(other), "%sother:", prefix);
    cwist_middleware_func isolated = limiter(pool, other, 1, 1);
    assert(request(isolated, fd) == 200 && request(isolated, fd) == 429);
    puts("[PASS] independent prefixes have independent limits");
    cwist_middleware_func fail_open = limiter(pool, prefix, 7, 0);
    assert(request(fail_open, -1) == 200);
    assert(request(shared, -1) == 503);
    const char *wrong_type[] = {"SET", key, "not-an-integer"};
    cwist_free(command(redis, 3, wrong_type));
    assert(request(shared, fd) == 503);
    assert(request(fail_open, fd) == 200);
    puts("[PASS] Redis command errors and missing peers obey failure policy");
    const char *remove[] = {"DEL", key};
    cwist_free(command(redis, 2, remove));
    cwist_middleware_func short_open = limiter_budget(pool, prefix, 7, 0, 25);
    cwist_middleware_func short_closed = limiter_budget(pool, prefix, 7, 1, 25);
    const char *pause[] = {"CLIENT", "PAUSE", "500", "ALL"};
    cwist_free(command(redis, 4, pause));
    double start = now_ms();
    assert(request(short_open, fd) == 200);
    assert(now_ms() - start < 150);
    start = now_ms();
    assert(request(short_closed, fd) == 503);
    assert(now_ms() - start < 150);
    puts("[PASS] paused Redis obeys bounded fail-open and fail-closed policies");
    test_inherited_warning(fail_open, fd);
    close(fd);
    cwist_redis_pool_destroy(pool);
    cwist_redis_close(redis);
    puts("All Redis rate limiter integration checks passed.");
    return 0;
}
