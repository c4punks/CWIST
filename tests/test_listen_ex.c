/**
 * @file test_listen_ex.c
 * @brief cwist_app_listen_ex() overrides and cwist_shutdown_request().
 *
 * Each case runs a real server in a forked child. The child sets
 * CWIST_WORKERS=4 and the opposite CWIST_C1M_MODE, so the checks only pass
 * if the explicit overrides win: with one worker every request is served by
 * the child itself, and a /stop request calling cwist_shutdown_request()
 * makes cwist_app_listen_ex() return 0.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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
        }                                                        \
    } while (0)

/* --- server side (child) ------------------------------------------------- */

static void pid_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)getpid());
    cwist_sstring_assign(res->body, buf);
}

static void stop_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Twice: a repeated request must be harmless. */
    cwist_shutdown_request();
    cwist_shutdown_request();
    cwist_sstring_assign(res->body, "stopping");
}

/* Serves `rounds` times in a row on the same port, resetting the shutdown
 * state in between, then exits with 0 if every listen returned 0. */
static void run_server(int port, int c1m, int rounds, int request_first) {
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    setenv("CWIST_WORKERS", "4", 1);
    setenv("CWIST_C1M_MODE", c1m ? "0" : "1", 1);
    /* C1M joins its handler threads before listen returns, so no drain is
     * needed before destroying the app. The classic pool's handler threads
     * are detached: keep a drain so /stop's own handler finishes first. */
    g_cwist_drain_timeout_sec = c1m ? 0 : 1;
    int result = 0;
    for (int i = 0; i < rounds; i++) {
        cwist_app *app = cwist_app_create();
        if (!app) _exit(2);
        cwist_app_get(app, "/pid", pid_handler);
        cwist_app_get(app, "/stop", stop_handler);
        if (request_first) cwist_shutdown_request();
        if (cwist_app_listen_ex(app, port, 1, c1m) != 0) result = 1;
        cwist_app_destroy(app);
        cwist_shutdown_reset();
    }
    _exit(result);
}

/* --- client side (parent) ------------------------------------------------ */

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

/* GET `path`; copies the body into `body`. Returns the HTTP status or -1. */
static int http_get(int port, const char *path, char *body, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    if (send(fd, req, (size_t)n, 0) != n) {
        close(fd);
        return -1;
    }
    char buf[4096];
    size_t used = 0;
    ssize_t got;
    while (used + 1 < sizeof(buf) && (got = recv(fd, buf + used, sizeof(buf) - 1 - used, 0)) > 0) {
        used += (size_t)got;
    }
    buf[used] = '\0';
    close(fd);
    int status = -1;
    if (sscanf(buf, "HTTP/1.1 %d", &status) != 1) return -1;
    const char *start = strstr(buf, "\r\n\r\n");
    if (body && cap) {
        snprintf(body, cap, "%s", start ? start + 4 : "");
    }
    return status;
}

static int wait_until_up(pid_t pid, int port) {
    for (int i = 0; i < 400; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        if (http_get(port, "/pid", NULL, 0) == 200) return 0;
        sleep_ms(25);
    }
    return -1;
}

/* Waits up to 10 s for the child; returns its exit code, or -1. */
static int wait_exit(pid_t pid) {
    for (int i = 0; i < 200; i++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        sleep_ms(50);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void test_serve_and_stop(int c1m) {
    int before = g_failures;
    const char *mode = c1m ? "C1M" : "classic";
    int port = pick_free_port();
    CHECK(port > 0, "no free port");
    if (port <= 0) return;
    const int rounds = 2;
    pid_t pid = fork();
    if (pid == 0) run_server(port, c1m, rounds, 0);
    CHECK(pid > 0, "fork failed");
    if (pid <= 0) return;

    for (int round = 0; round < rounds; round++) {
        if (wait_until_up(pid, port) != 0) {
            CHECK(0, "%s round %d: server did not come up", mode, round);
            break;
        }
        /* One worker: every request is served by the child itself, although
         * CWIST_WORKERS=4 is set. */
        for (int i = 0; i < 12; i++) {
            char body[64] = {0};
            int status = http_get(port, "/pid", body, sizeof(body));
            CHECK(status == 200 && atoi(body) == (int)pid,
                  "%s round %d: request %d served by pid %s, want %d", mode, round, i, body,
                  (int)pid);
        }
        char body[64] = {0};
        CHECK(http_get(port, "/stop", body, sizeof(body)) == 200 && strcmp(body, "stopping") == 0,
              "%s round %d: /stop failed", mode, round);
    }
    CHECK(wait_exit(pid) == 0, "%s: listen did not return 0 after shutdown requests", mode);
    if (g_failures == before) {
        printf("Passed %s: one worker, shutdown request, listen again after reset\n", mode);
    }
}

static void test_request_before_listen(void) {
    int before = g_failures;
    int port = pick_free_port();
    CHECK(port > 0, "no free port");
    if (port <= 0) return;
    pid_t pid = fork();
    if (pid == 0) run_server(port, 1, 1, 1);
    CHECK(pid > 0, "fork failed");
    if (pid <= 0) return;
    CHECK(wait_exit(pid) == 0, "a request made before listen did not make it return 0");
    if (g_failures == before) printf("Passed shutdown requested before listen returns at once\n");
}

int main(void) {
    printf("Testing cwist_app_listen_ex() and cwist_shutdown_request()...\n");
    test_serve_and_stop(1);
    test_serve_and_stop(0);
    test_request_before_listen();
    if (g_failures) {
        fprintf(stderr, "test_listen_ex: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All listen_ex tests passed!\n");
    return 0;
}
