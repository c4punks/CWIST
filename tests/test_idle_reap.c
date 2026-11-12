/**
 * @file test_idle_reap.c
 * @brief C1M idle keep-alive connections are closed by the sweep timer.
 *
 * Before the sweep, http_async_event_cb() only reaped an idle connection when
 * an event arrived, so a client that went quiet held its socket forever. A
 * forked child serves with CWIST_C1M_MODE and CWIST_HTTP_KEEP_ALIVE_TIMEOUT=1;
 * the parent checks that:
 *   1. a connection that never sends a byte is closed,
 *   2. a keep-alive connection that goes quiet after one request is closed,
 *   3. a connection that keeps sending requests inside the timeout survives.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

static int g_failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
            g_failures++;                                        \
        } else {                                                 \
            printf("  ok   ");                                   \
            printf(__VA_ARGS__);                                 \
            putchar('\n');                                       \
        }                                                        \
    } while (0)

static void ok_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "ok");
}

static void run_server(int port) {
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    setenv("CWIST_HTTP_KEEP_ALIVE_TIMEOUT", "1", 1);
    cwist_app *app = cwist_app_create();
    if (!app) _exit(2);
    cwist_app_get(app, "/", ok_handler);
    cwist_app_listen_ex(app, port, 2, 1);
    _exit(0);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send one keep-alive GET and read until the response body "ok" arrives. */
static int do_request(int fd) {
    static const char req[] = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    if (send(fd, req, sizeof(req) - 1, 0) != (ssize_t)(sizeof(req) - 1)) return -1;
    char buf[1024];
    size_t got = 0;
    double deadline = now_s() + 3.0;
    while (now_s() < deadline && got < sizeof(buf) - 1) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end && strstr(hdr_end, "ok")) return 0;
    }
    return -1;
}

/* Seconds until the server closes @p fd, or -1 if it stays open past @p max_s. */
static double wait_closed(int fd, double max_s) {
    double start = now_s();
    char buf[256];
    while (now_s() - start < max_s) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) continue;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) return now_s() - start;
    }
    return -1;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int port = 21000 + (int)(getpid() % 20000);
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) run_server(port);

    int probe = -1;
    for (int i = 0; i < 100 && probe < 0; i++) {
        probe = connect_port(port);
        if (probe < 0) usleep(50 * 1000);
    }
    if (probe < 0) {
        fprintf(stderr, "server did not come up on port %d\n", port);
        kill(child, SIGKILL);
        return 1;
    }
    close(probe);

    printf("C1M idle keep-alive sweep (timeout 1 s)\n");

    int silent = connect_port(port);
    int quiet = connect_port(port);
    int busy = connect_port(port);
    CHECK(silent >= 0 && quiet >= 0 && busy >= 0, "three client connections open");

    CHECK(do_request(quiet) == 0, "keep-alive request served before going quiet");

    /* Keep `busy` active every 400 ms for ~3 s while the other two idle. */
    int busy_ok = 1;
    double t0 = now_s();
    while (now_s() - t0 < 3.0) {
        if (do_request(busy) != 0) busy_ok = 0;
        usleep(400 * 1000);
    }
    CHECK(busy_ok, "connection with requests inside the timeout stays open");

    double s1 = wait_closed(silent, 5.0);
    CHECK(s1 >= 0, "connection that never sent a byte is closed (%.1fs after the check)", s1);
    double s2 = wait_closed(quiet, 5.0);
    CHECK(s2 >= 0, "keep-alive connection idle after one request is closed (%.1fs)", s2);

    close(silent);
    close(quiet);
    close(busy);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all idle reap checks passed\n");
    return 0;
}
