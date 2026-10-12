/**
 * @file test_classic_shutdown.c
 * @brief Classic-pool shutdown never destroys the app under a live handler.
 *
 * Classic pool handlers run on detached workers and used to outlive the
 * drain timeout on keep-alive connections; cwist_app_destroy() then freed the
 * route table while a worker was still dispatching a request. Each case runs
 * a classic server (one worker, CWIST_C1M_MODE=0) in a forked child, asks it
 * to stop through /stop, and checks that it exits cleanly and in time:
 *
 *   1. An idle keep-alive connection is closed at once; it does not hold
 *      shutdown for the drain timeout.
 *   2. A request already being handled (/slow) still gets its full response.
 *   3. A client that keeps sending requests on its keep-alive connection
 *      cannot keep the server alive; every response it gets is complete.
 *
 * The UAF itself shows up under ASan/TSan, which run this test too.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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

/* Drain timeout the child runs with. An idle connection must not cost it. */
#define DRAIN_SEC 4

/* --- server side (child) ------------------------------------------------- */

static void ok_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "ok");
}

static void slow_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    cwist_sstring_assign(res->body, "slow-done");
}

static void stop_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_shutdown_request();
    cwist_sstring_assign(res->body, "stopping");
}

static void run_server(int port) {
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    setenv("CWIST_WORKERS", "1", 1);
    setenv("CWIST_C1M_MODE", "0", 1);
    g_cwist_drain_timeout_sec = DRAIN_SEC;
    cwist_app *app = cwist_app_create();
    if (!app) _exit(2);
    cwist_app_get(app, "/", ok_handler);
    cwist_app_get(app, "/slow", slow_handler);
    cwist_app_get(app, "/stop", stop_handler);
    int rc = cwist_app_listen_ex(app, port, 1, 0);
    cwist_app_destroy(app);
    _exit(rc == 0 ? 0 : 1);
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

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One keep-alive request on `fd`. Returns 1 when a complete response with
 * body `want` arrived, 0 when the connection closed before any byte of a
 * response, -1 on a partial or wrong response. */
static int request(int fd, const char *path, const char *want) {
    char req[128];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", path);
    if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) return 0;
    char buf[2048];
    size_t used = 0;
    for (;;) {
        ssize_t got = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (got <= 0) return used == 0 ? 0 : -1;
        used += (size_t)got;
        buf[used] = '\0';
        const char *hdr_end = strstr(buf, "\r\n\r\n");
        const char *cl = strstr(buf, "Content-Length:");
        if (!hdr_end || !cl) {
            if (used + 1 >= sizeof(buf)) return -1;
            continue;
        }
        size_t len = (size_t)strtoul(cl + 15, NULL, 10);
        size_t have = used - (size_t)(hdr_end + 4 - buf);
        if (have < len) continue;
        if (strncmp(buf, "HTTP/1.1 200", 12) != 0) return -1;
        return (len == strlen(want) && memcmp(hdr_end + 4, want, len) == 0) ? 1 : -1;
    }
}

static int wait_until_up(pid_t pid, int port) {
    for (int i = 0; i < 400; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        int fd = connect_to(port);
        if (fd >= 0) {
            int ok = request(fd, "/", "ok");
            close(fd);
            if (ok == 1) return 0;
        }
        sleep_ms(25);
    }
    return -1;
}

/* Waits up to `limit_s` for the child; returns its exit code, or -1. */
static int wait_exit(pid_t pid, double limit_s) {
    double end = now_s() + limit_s;
    while (now_s() < end) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static pid_t start_server(int *port_out) {
    int port = pick_free_port();
    CHECK(port > 0, "no free port");
    if (port <= 0) return -1;
    pid_t pid = fork();
    if (pid == 0) run_server(port);
    CHECK(pid > 0, "fork failed");
    if (pid <= 0) return -1;
    if (wait_until_up(pid, port) != 0) {
        CHECK(0, "server did not come up");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    *port_out = port;
    return pid;
}

static void send_stop(int port) {
    int fd = connect_to(port);
    CHECK(fd >= 0, "connect for /stop failed");
    if (fd < 0) return;
    CHECK(request(fd, "/stop", "stopping") == 1, "/stop did not answer");
    close(fd);
}

static void test_idle_keepalive_does_not_hold_shutdown(void) {
    int before = g_failures;
    int port;
    pid_t pid = start_server(&port);
    if (pid < 0) return;

    int idle = connect_to(port);
    CHECK(idle >= 0 && request(idle, "/", "ok") == 1, "keep-alive request failed");

    double t0 = now_s();
    send_stop(port);
    int rc = wait_exit(pid, DRAIN_SEC + 10);
    double took = now_s() - t0;
    CHECK(rc == 0, "server exit code %d with an idle keep-alive connection", rc);
    CHECK(took < DRAIN_SEC - 1.0, "shutdown took %.2f s; an idle connection held the %d s drain",
          took, DRAIN_SEC);
    char b;
    CHECK(idle < 0 || recv(idle, &b, 1, 0) == 0, "idle connection was not closed");
    if (idle >= 0) close(idle);
    if (g_failures == before) printf("Passed idle keep-alive closed at shutdown (%.2f s)\n", took);
}

typedef struct {
    int port;
    int result;
} slow_arg_t;

static void *slow_client(void *p) {
    slow_arg_t *a = p;
    int fd = connect_to(a->port);
    a->result = fd >= 0 ? request(fd, "/slow", "slow-done") : -2;
    if (fd >= 0) close(fd);
    return NULL;
}

static void test_inflight_request_completes(void) {
    int before = g_failures;
    int port;
    pid_t pid = start_server(&port);
    if (pid < 0) return;

    slow_arg_t arg = {.port = port, .result = -3};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, slow_client, &arg) == 0, "pthread_create failed");
    sleep_ms(300); /* /slow is now inside its handler */
    send_stop(port);
    pthread_join(t, NULL);
    CHECK(arg.result == 1, "in-flight /slow response was %d, want complete", arg.result);
    CHECK(wait_exit(pid, DRAIN_SEC + 10) == 0, "server did not exit cleanly after /slow");
    if (g_failures == before) printf("Passed in-flight request finishes during shutdown\n");
}

typedef struct {
    int port;
    _Atomic int stop;
    int served;
    int bad;
} busy_arg_t;

static void *busy_client(void *p) {
    busy_arg_t *a = p;
    int fd = connect_to(a->port);
    if (fd < 0) {
        a->bad++;
        return NULL;
    }
    while (!atomic_load(&a->stop)) {
        int r = request(fd, "/", "ok");
        if (r == 1) {
            a->served++;
        } else {
            if (r < 0) a->bad++;
            break; /* server closed the connection */
        }
    }
    close(fd);
    return NULL;
}

static void test_busy_keepalive_does_not_keep_server_alive(void) {
    int before = g_failures;
    int port;
    pid_t pid = start_server(&port);
    if (pid < 0) return;

    busy_arg_t arg = {.port = port};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, busy_client, &arg) == 0, "pthread_create failed");
    sleep_ms(200);
    double t0 = now_s();
    send_stop(port);
    int rc = wait_exit(pid, DRAIN_SEC + 10);
    double took = now_s() - t0;
    atomic_store(&arg.stop, 1);
    pthread_join(t, NULL);
    CHECK(rc == 0, "server exit code %d with a busy keep-alive client", rc);
    CHECK(took < DRAIN_SEC - 1.0, "shutdown took %.2f s behind a busy keep-alive client", took);
    CHECK(arg.served > 0, "busy client was never served");
    CHECK(arg.bad == 0, "busy client got %d partial or wrong response(s)", arg.bad);
    if (g_failures == before)
        printf("Passed busy keep-alive client stopped at shutdown (%d served, %.2f s)\n",
               arg.served, took);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("Testing classic-pool shutdown with live connections...\n");
    test_idle_keepalive_does_not_hold_shutdown();
    test_inflight_request_completes();
    test_busy_keepalive_does_not_keep_server_alive();
    if (g_failures) {
        fprintf(stderr, "test_classic_shutdown: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All classic shutdown tests passed!\n");
    return 0;
}
