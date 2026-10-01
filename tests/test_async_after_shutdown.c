/**
 * @file test_async_after_shutdown.c
 * @brief A deferred response completed after the server has stopped.
 *
 * A handler defers its response and the server shuts down before anything
 * answers it. cwist_app_listen_ex() then destroys the C1M worker reactor the
 * connection was parked on. Completing the cwist_async afterwards (respond,
 * or the timeout firing) must close the connection without touching that
 * freed reactor; ASan reports a heap use-after-free otherwise.
 *
 * The server runs in-process on a thread, so the handle outlives listen.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/net/http/async.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Always evaluated, unlike assert(). */
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                        \
        }                                                                   \
    } while (0)

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    abort();
}

static _Atomic(cwist_async *) g_handle = NULL;
static atomic_ulong g_timeout_ms = 0;

/* Defers and publishes the handle; nothing answers it while serving. */
static void defer_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    REQUIRE(a);
    unsigned long ms = atomic_load(&g_timeout_ms);
    if (ms) cwist_async_set_timeout(a, ms);
    atomic_store(&g_handle, a);
}

static void ok_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "ok");
}

typedef struct {
    cwist_app *app;
    int port;
    int rc;
} serve_args;

static void *serve(void *arg) {
    serve_args *s = (serve_args *)arg;
    s->rc = cwist_app_listen_ex(s->app, s->port, 1, 1);
    return NULL;
}

static int pick_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    int port = -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* Connects and sends GET @p path, retrying until the server is up. */
static int send_get(int port, const char *path, bool close_after) {
    for (int attempt = 0; attempt < 200; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            char req[256];
            int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: localhost\r\n%s\r\n", path,
                             close_after ? "Connection: close\r\n" : "");
            REQUIRE(send(fd, req, (size_t)n, 0) == n);
            return fd;
        }
        close(fd);
        sleep_ms(25);
    }
    fail("server did not come up");
    return -1;
}

/* Reads until EOF; returns the number of bytes received. */
static size_t read_all(int fd, char *buf, size_t cap) {
    size_t used = 0;
    ssize_t got;
    while (used + 1 < cap && (got = recv(fd, buf + used, cap - 1 - used, 0)) > 0) {
        used += (size_t)got;
    }
    buf[used] = '\0';
    return used;
}

static cwist_async *wait_for_handle(void) {
    for (int i = 0; i < 400; i++) {
        cwist_async *a = atomic_exchange(&g_handle, NULL);
        if (a) return a;
        sleep_ms(10);
    }
    fail("the handler never deferred");
    return NULL;
}

/* Serves, defers one request, stops the server and returns the pending handle
 * together with the client socket of the deferred request. */
static cwist_async *defer_then_stop(unsigned long timeout_ms, int *client_fd) {
    atomic_store(&g_timeout_ms, timeout_ms);
    cwist_app *app = cwist_app_create();
    REQUIRE(app);
    cwist_app_get(app, "/defer", defer_handler);
    cwist_app_get(app, "/ok", ok_handler);
    serve_args args = {app, pick_free_port(), -1};
    REQUIRE(args.port > 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, serve, &args) == 0);

    /* The server still answers ordinary requests while one is deferred. */
    *client_fd = send_get(args.port, "/defer", false);
    cwist_async *a = wait_for_handle();
    char buf[1024];
    int ok_fd = send_get(args.port, "/ok", true);
    REQUIRE(read_all(ok_fd, buf, sizeof(buf)) > 0 && strstr(buf, "HTTP/1.1 200"));
    close(ok_fd);

    cwist_shutdown_request();
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(args.rc == 0);
    /* listen has returned: the worker reactors are freed. */
    cwist_app_destroy(app);
    cwist_shutdown_reset();
    return a;
}

static void test_respond_after_stop(void) {
    int client;
    cwist_async *a = defer_then_stop(0, &client);
    /* The exchange was still pending, so this call claims it; with the
     * reactor gone it closes the connection instead of answering. */
    REQUIRE(cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "late", 4));
    char buf[1024];
    REQUIRE(read_all(client, buf, sizeof(buf)) == 0);
    close(client);
    printf("Passed respond after the server stopped closes the connection\n");
}

static void test_abort_after_stop(void) {
    int client;
    cwist_async *a = defer_then_stop(0, &client);
    REQUIRE(cwist_async_abort(a, CWIST_HTTP_INTERNAL_ERROR));
    char buf[1024];
    REQUIRE(read_all(client, buf, sizeof(buf)) == 0);
    close(client);
    printf("Passed abort after the server stopped closes the connection\n");
}

static void test_timeout_after_stop(void) {
    int client;
    /* Fires only after listen has returned (an idle shutdown takes a few
     * hundred milliseconds at most), well inside the client's 5 s read. */
    cwist_async *a = defer_then_stop(1500, &client);
    (void)a; /* The timeout owns completion now. */
    char buf[1024];
    REQUIRE(read_all(client, buf, sizeof(buf)) == 0);
    close(client);
    printf("Passed a timeout firing after the server stopped closes the connection\n");
}

int main(void) {
    printf("Testing deferred responses completed after shutdown...\n");
    g_cwist_drain_timeout_sec = 0;
    test_respond_after_stop();
    test_abort_after_stop();
    test_timeout_after_stop();
    printf("All deferred-after-shutdown tests passed.\n");
    return 0;
}
