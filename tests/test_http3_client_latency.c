/**
 * @file test_http3_client_latency.c
 * @brief Requests on an established HTTP/3 client connection must not wait
 *        for the engine's next advisory tick.
 *
 * The request path used to wait in the I/O loop before checking whether
 * lsquic had already created the stream (it does so synchronously on an
 * established connection), so every request paid about one tick (~1 ms)
 * even on loopback. The median of sequential GETs against an in-process
 * server must now be well under a tick; before the fix it was about 1.1 ms.
 * The limit leaves room for sanitizer builds.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http3_client.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 19963
#define REQUESTS 300
#define LIMIT_US 800.0

static _Thread_local int fail_alloc;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (fail_alloc) return NULL;
    return __real_malloc(size);
}
static _Thread_local int fail_header;
static _Thread_local int fail_client_body;
static _Atomic int fail_server_body;
static _Atomic int fail_server_header;
static _Atomic int fail_server_path;
static _Atomic int handler_calls;
cwist_error_t __real_cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                           const char *value);
cwist_error_t __wrap_cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                           const char *value) {
    if (fail_header || (fail_server_header && strcmp(key, "host") == 0))
        return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_http_header_add(head, key, value);
}
cwist_error_t __real_cwist_sstring_assign(cwist_sstring *str, const char *data);
cwist_error_t __wrap_cwist_sstring_assign(cwist_sstring *str, const char *data) {
    if (fail_server_path && data && strcmp(data, "/") == 0)
        return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_sstring_assign(str, data);
}
cwist_error_t __real_cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);
cwist_error_t __wrap_cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len) {
    if (fail_client_body || (fail_server_body && len == 5 && memcmp(data, "hello", 5) == 0))
        return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_sstring_assign_len(str, data, len);
}

static void get_handler(void *user, cwist_http_request *req, cwist_http_response *res) {
    (void)user;
    (void)req;
    atomic_fetch_add(&handler_calls, 1);
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "pong");
}

typedef struct {
    int fd;
    cwist_http3_context *ctx;
} server_t;

static void *server_thread(void *arg) {
    server_t *s = arg;
    cwist_http3_server_loop(s->fd, s->ctx, get_handler, NULL);
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void) {
    server_t srv = {0};
    if (cwist_http3_init_context_ephemeral(&srv.ctx).error.err_i16 != 0) return 1;
    srv.fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (srv.fd < 0 || bind(srv.fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &srv);
    usleep(100000);

    cwist_http3_client *c = cwist_http3_client_create();
    cwist_http3_client_set_insecure(c, 1);
    cwist_http3_client_set_server(c, "127.0.0.1", PORT);
    cwist_http3_client_set_timeout_ms(c, 3000);

    int failures = 0, ok = 0;
    cwist_http_response *res = NULL;
    cwist_error_t e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    if (e.error.err_i16 != 0 || !res) return 1;
    cwist_http_response_destroy(res);
    fail_alloc = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    fail_alloc = 0;
    if (res) {
        cwist_http_response_destroy(res);
        return 1;
    }
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    if (e.error.err_i16 != 0 || !res) return 1;
    cwist_http_response_destroy(res);

    fail_header = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    fail_header = 0;
    if (res) {
        cwist_http_response_destroy(res);
        return 1;
    }
    fail_client_body = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    fail_client_body = 0;
    if (res) {
        cwist_http_response_destroy(res);
        return 1;
    }
    int before = atomic_load(&handler_calls);
    fail_server_body = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_POST, NULL, "hello", 5, &res);
    fail_server_body = 0;
    if (e.error.err_i16 != 0 || !res || res->status_code != CWIST_HTTP_INTERNAL_ERROR ||
        atomic_load(&handler_calls) != before)
        return 1;
    cwist_http_response_destroy(res);

    before = atomic_load(&handler_calls);
    fail_server_header = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    fail_server_header = 0;
    if (e.error.err_i16 != 0 || !res || res->status_code != CWIST_HTTP_BAD_REQUEST ||
        atomic_load(&handler_calls) != before)
        return 1;
    cwist_http_response_destroy(res);

    before = atomic_load(&handler_calls);
    fail_server_path = 1;
    res = NULL;
    e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    fail_server_path = 0;
    if (e.error.err_i16 != 0 || !res || res->status_code != CWIST_HTTP_BAD_REQUEST ||
        atomic_load(&handler_calls) != before)
        return 1;
    cwist_http_response_destroy(res);
    static double lat[REQUESTS];
    for (int i = 0; i < REQUESTS; i++) {
        double t0 = now_us();
        cwist_http_response *res = NULL;
        cwist_error_t e = cwist_http3_client_request(c, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
        double dt = now_us() - t0;
        if (e.error.err_i16 == 0 && res && res->status_code == CWIST_HTTP_OK) {
            if (i > 0) lat[ok++] = dt; /* skip the handshake */
        }
        if (res) cwist_http_response_destroy(res);
    }
    qsort(lat, (size_t)ok, sizeof(lat[0]), cmp_double);
    double median = ok ? lat[ok / 2] : 1e18;
    printf("%d/%d requests ok, median %.0f us\n", ok, REQUESTS - 1, median);
    if (ok != REQUESTS - 1) {
        fprintf(stderr, "FAIL: %d request(s) failed\n", REQUESTS - 1 - ok);
        failures++;
    }
    if (median >= LIMIT_US) {
        fprintf(stderr, "FAIL: median request took %.0f us (limit %.0f): requests wait for a tick\n",
                median, LIMIT_US);
        failures++;
    }

    cwist_http3_client_destroy(c);
    __atomic_store_n(&srv.ctx->running, 0, __ATOMIC_RELEASE);
    pthread_join(tid, NULL);
    close(srv.fd);
    cwist_http3_destroy_context(srv.ctx);
    if (failures) return 1;
    printf("HTTP/3 client latency test passed\n");
    return 0;
}
