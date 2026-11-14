#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/https.h>
#include "../src/net/http/async_internal.h"
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";
static const char LARGE_BODY[] = "abcdefghijklmnopqrstuvwxyz0123456789";

typedef struct {
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
} client_t;

typedef struct {
    char *wire;
    char *body;
    size_t body_len;
} response_t;

static void hello_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "hello");
}

static void large_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    enum { LARGE_SIZE = 512 * 1024 };
    char *body = cwist_alloc(LARGE_SIZE + 1);
    assert(body != NULL);
    for (size_t i = 0; i < LARGE_SIZE; i++) body[i] = LARGE_BODY[i % (sizeof(LARGE_BODY) - 1)];
    cwist_sstring_adopt_len(res->body, body, LARGE_SIZE);
}

static void *defer_worker(void *arg) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
    assert(cwist_async_respond((cwist_async *)arg, CWIST_HTTP_OK, "text/plain", "deferred", 8));
    return NULL;
}

static void defer_route(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    assert(a != NULL);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, defer_worker, a) == 0);
    pthread_detach(thread);
}

static cwist_app *make_app(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_error_t err = cwist_app_use_https(app, TEST_CERT, TEST_KEY);
    assert(cwist_error_is_ok(&err));
    cwist_app_get(app, "/hello", hello_route);
    cwist_app_get(app, "/large", large_route);
    cwist_app_get(app, "/defer", defer_route);
    return app;
}

static client_t client_open(cwist_app *app) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int send_buffer = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    https_pool_submit(sv[0], app->ssl_ctx, app->https_request_handler, app);

    client_t client = {.fd = sv[1]};
    struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    assert(setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    client.ctx = SSL_CTX_new(TLS_client_method());
    assert(client.ctx != NULL);
    SSL_CTX_set_verify(client.ctx, SSL_VERIFY_NONE, NULL);
    client.ssl = SSL_new(client.ctx);
    assert(client.ssl != NULL);
    assert(SSL_set_fd(client.ssl, client.fd) == 1);
    assert(SSL_connect(client.ssl) == 1);
    return client;
}

static void ssl_write_all(SSL *ssl, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, data + off, (int)(len - off));
        assert(n > 0);
        off += (size_t)n;
    }
}

static response_t read_response(client_t *client) {
    size_t cap = 64 * 1024;
    size_t len = 0;
    char *wire = cwist_alloc(cap + 1);
    assert(wire != NULL);
    size_t expected = 0;
    char *body = NULL;
    for (;;) {
        size_t needed = body ? len + expected - (len - (size_t)(body - wire)) : len + 1;
        if (needed > cap) {
            size_t body_off = body ? (size_t)(body - wire) : 0;
            while (cap < needed) cap *= 2;
            char *larger = cwist_realloc(wire, cap + 1);
            assert(larger != NULL);
            wire = larger;
            if (body) body = wire + body_off;
        }
        size_t read_size = 1;
        if (body) {
            size_t body_have = len - (size_t)(body - wire);
            read_size = expected - body_have;
            assert(read_size > 0 && read_size <= cap - len);
        }
        int n = SSL_read(client->ssl, wire + len, (int)read_size);
        assert(n > 0);
        len += (size_t)n;
        wire[len] = '\0';
        if (!body) {
            char *end = strstr(wire, "\r\n\r\n");
            if (!end) continue;
            assert(strncmp(wire, "HTTP/1.1 200", 12) == 0);
            char *content_length = strcasestr(wire, "\r\nContent-Length:");
            assert(content_length != NULL && content_length < end);
            expected = (size_t)strtoul(content_length + 17, NULL, 10);
            body = end + 4;
        }
        if (len - (size_t)(body - wire) >= expected) break;
    }
    response_t response = {.wire = wire, .body = body, .body_len = expected};
    return response;
}

static void free_response(response_t *response) {
    cwist_free(response->wire);
    memset(response, 0, sizeof(*response));
}

static void expect_body(response_t *response, const char *expected) {
    size_t len = strlen(expected);
    assert(response->body_len == len);
    assert(memcmp(response->body, expected, len) == 0);
}

static void request_hello(client_t *client) {
    static const char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssl_write_all(client->ssl, request, sizeof(request) - 1);
    response_t response = read_response(client);
    expect_body(&response, "hello");
    free_response(&response);
}

static void test_split_records_and_pipeline(client_t *client) {
    static const char first[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char second[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    for (size_t i = 0; i < sizeof(first) - 1; i++) ssl_write_all(client->ssl, first + i, 1);
    ssl_write_all(client->ssl, second, sizeof(second) - 1);
    response_t one = read_response(client);
    response_t two = read_response(client);
    expect_body(&one, "hello");
    expect_body(&two, "hello");
    free_response(&one);
    free_response(&two);
    assert(cwist_http_inflight_count() > 0);
}

static void test_deferred_keep_alive(client_t *client) {
    static const char request[] = "GET /defer HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssl_write_all(client->ssl, request, sizeof(request) - 1);
    response_t response = read_response(client);
    expect_body(&response, "deferred");
    free_response(&response);
    request_hello(client);
}

static void test_large_response(client_t *client) {
    static const char request[] =
        "GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssl_write_all(client->ssl, request, sizeof(request) - 1);
    response_t response = read_response(client);
    assert(response.body_len == 512 * 1024);
    for (size_t i = 0; i < response.body_len; i++) {
        assert(response.body[i] == LARGE_BODY[i % (sizeof(LARGE_BODY) - 1)]);
    }
    free_response(&response);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setenv("CWIST_WORKER_THREADS", "1", 1);
    setenv("CWIST_HTTP_KEEP_ALIVE_TIMEOUT", "1", 1);
    setenv("CWIST_HTTPS_IDLE_TIMEOUT_MS", "30000", 1);
#ifdef TEST_HTTPS_REACTOR_FULL_GC
    cwist_full_gc(true);
#endif
    assert(https_pool_init() == 0);
    assert(cwist_http_pool_init_mode(true) == 0);
    cwist_app *app = make_app();

    client_t split = client_open(app);
    test_split_records_and_pipeline(&split);
    usleep(1500000);
    request_hello(&split);
    test_deferred_keep_alive(&split);
    SSL_shutdown(split.ssl);
    SSL_free(split.ssl);
    SSL_CTX_free(split.ctx);
    close(split.fd);

    client_t large = client_open(app);
    test_large_response(&large);
    SSL_free(large.ssl);
    SSL_CTX_free(large.ctx);
    close(large.fd);

    for (int i = 0; i < 200 && cwist_http_inflight_count() > 0; i++) usleep(10000);
    assert(cwist_http_inflight_count() == 0);
    https_pool_destroy();
    cwist_http_pool_destroy();
    cwist_app_destroy(app);
    printf("HTTPS reactor tests passed\n");
    return 0;
}
