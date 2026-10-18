/**
 * @file test_socket_handoff.c
 * @brief Zero-downtime restart via SO_REUSEPORT socket handoff.
 *
 * Scaled-down local version of the issue #405 C10k restart scenario: server
 * process A holds N keep-alive connections, server process B binds the same
 * port (SO_REUSEPORT overlap), A is signalled to drain (SIGTERM), and the
 * test asserts that every request still gets a response -- served by A while
 * it drains, or by B after the client reconnects onto A's closed connection
 * -- and that new connections land on B once A is gone.
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

#define HELD_CONNS 256
#define FRESH_CONNS 64

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

/* --- server side (children) ---------------------------------------------- */

static void pid_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)getpid());
    cwist_sstring_assign(res->body, buf);
}

static void run_server(int port) {
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    g_cwist_drain_timeout_sec = 2;
    cwist_app *app = cwist_app_create();
    if (!app) _exit(2);
    cwist_app_get(app, "/pid", pid_handler);
    /* One worker: every request is served by this process, so the /pid body
     * identifies which generation answered. */
    int rc = cwist_app_listen_ex(app, port, 1, -1);
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

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One keep-alive GET /pid round on an already-connected fd. Returns the
 * serving pid, 0 when the connection was closed or the answer broke. */
static int held_get_pid(int fd) {
    static const char req[] = "GET /pid HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (send(fd, req, sizeof(req) - 1, MSG_NOSIGNAL) != (ssize_t)(sizeof(req) - 1)) return 0;
    char buf[4096];
    /* Headers first: read until the header terminator. */
    size_t used = 0;
    for (;;) {
        if (used + 1 >= sizeof(buf)) return 0;
        ssize_t got = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (got <= 0) return 0;
        used += (size_t)got;
        buf[used] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (!end) continue;
        int status = 0;
        if (sscanf(buf, "HTTP/1.1 %d", &status) != 1 || status != 200) return 0;
        int clen = -1;
        const char *cl = strstr(buf, "Content-Length:");
        if (cl) clen = atoi(cl + 15);
        if (clen < 0) return 0;
        size_t body_off = (size_t)(end + 4 - buf);
        while (used - body_off < (size_t)clen) {
            got = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
            if (got <= 0) return 0;
            used += (size_t)got;
        }
        buf[body_off + (size_t)clen] = '\0';
        return atoi(buf + body_off);
    }
}

/* Fresh-connection GET /pid; returns the serving pid or 0. */
static int fresh_get_pid(int port) {
    int fd = connect_to(port);
    if (fd < 0) return 0;
    int pid = held_get_pid(fd);
    close(fd);
    return pid;
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

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("Testing zero-downtime restart via socket handoff...\n");

    int port = pick_free_port();
    CHECK(port > 0, "no free port");
    if (port <= 0) return 1;

    pid_t pid_a = fork();
    if (pid_a == 0) run_server(port);
    CHECK(pid_a > 0, "fork A failed");
    if (pid_a <= 0) return 1;

    /* A up: with one bound process every connection lands on A. */
    int up = 0;
    for (int i = 0; i < 400 && !up; i++) {
        int status;
        if (waitpid(pid_a, &status, WNOHANG) == pid_a) break;
        up = fresh_get_pid(port) == (int)pid_a;
        if (!up) sleep_ms(25);
    }
    CHECK(up, "server A did not come up");
    if (!up) {
        kill(pid_a, SIGKILL);
        waitpid(pid_a, NULL, 0);
        return 1;
    }

    /* Hold HELD_CONNS keep-alive connections against A. */
    int held[HELD_CONNS];
    int n_held = 0;
    for (int i = 0; i < HELD_CONNS; i++) {
        held[i] = connect_to(port);
        if (held[i] < 0) break;
        if (held_get_pid(held[i]) != (int)pid_a) {
            close(held[i]);
            break;
        }
        n_held++;
    }
    CHECK(n_held == HELD_CONNS, "only %d/%d held connections established", n_held, HELD_CONNS);

    /* B binds the same port while A is up: the SO_REUSEPORT overlap window. */
    pid_t pid_b = fork();
    if (pid_b == 0) run_server(port);
    CHECK(pid_b > 0, "fork B failed");
    if (pid_b <= 0) {
        kill(pid_a, SIGKILL);
        waitpid(pid_a, NULL, 0);
        return 1;
    }
    int b_up = 0;
    for (int i = 0; i < 400 && !b_up; i++) {
        int status;
        if (waitpid(pid_b, &status, WNOHANG) == pid_b) break;
        /* During the overlap new connections hash to A or B; poll until a
         * fresh connection proves B accepts. */
        b_up = fresh_get_pid(port) == (int)pid_b;
        if (!b_up) sleep_ms(25);
    }
    CHECK(b_up, "server B did not come up during the overlap window");

    /* Handoff: signal A to drain. */
    kill(pid_a, SIGTERM);

    int served_by_a = 0, reconnected = 0, failed = 0;
    for (int i = 0; i < n_held; i++) {
        int pid = held_get_pid(held[i]);
        close(held[i]);
        if (pid == (int)pid_a) {
            served_by_a++;
            continue;
        }
        /* A closed the connection while draining: the client reconnects and
         * must land on B -- no request may be lost for good. */
        reconnected++;
        if (fresh_get_pid(port) != (int)pid_b) failed++;
    }
    CHECK(failed == 0, "%d request(s) failed even after reconnect", failed);

    CHECK(wait_exit(pid_a) == 0, "server A did not drain and exit cleanly");

    /* With A gone, every new connection must reach B. */
    int wrong = 0;
    for (int i = 0; i < FRESH_CONNS; i++) {
        if (fresh_get_pid(port) != (int)pid_b) wrong++;
    }
    CHECK(wrong == 0, "%d/%d fresh connections did not reach B", wrong, FRESH_CONNS);

    kill(pid_b, SIGTERM);
    CHECK(wait_exit(pid_b) == 0, "server B did not shut down cleanly");

    printf("handoff: %d held connections, %d answered by draining A, %d reconnected to B, "
           "%d failed\n",
           n_held, served_by_a, reconnected, failed);

    if (g_failures) {
        fprintf(stderr, "test_socket_handoff: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All socket handoff tests passed!\n");
    return 0;
}
