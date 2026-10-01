/**
 * @file test_listen_signal_restore.c
 * @brief cwist_app_listen_ex() handles SIGTERM/SIGINT only while it serves.
 *
 * The process installs its own SIGTERM and SIGINT handlers first. While the
 * server runs, a SIGTERM must stop it gracefully (CWIST's handler); once
 * cwist_app_listen_ex() has returned, both signals must reach the process's
 * own handlers again instead of being swallowed as a shutdown request.
 *
 * The server runs in-process on a thread so the signals reach both sides.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
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

static volatile sig_atomic_t g_own_term = 0;
static volatile sig_atomic_t g_own_int = 0;

static void own_handler(int sig) {
    if (sig == SIGTERM) g_own_term++;
    if (sig == SIGINT) g_own_int++;
}

static void install_own_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = own_handler;
    REQUIRE(sigaction(SIGTERM, &sa, NULL) == 0);
    REQUIRE(sigaction(SIGINT, &sa, NULL) == 0);
}

/* True when both signals are handled by own_handler right now. */
static bool own_handlers_installed(void) {
    struct sigaction term, intr;
    REQUIRE(sigaction(SIGTERM, NULL, &term) == 0);
    REQUIRE(sigaction(SIGINT, NULL, &intr) == 0);
    return term.sa_handler == own_handler && intr.sa_handler == own_handler;
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

/* Waits until the server accepts connections (and so has installed its
 * handlers, which happens before it binds). */
static void wait_until_up(int port) {
    for (int attempt = 0; attempt < 400; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
        if (rc == 0) return;
        sleep_ms(25);
    }
    fprintf(stderr, "FAIL: server did not come up\n");
    abort();
}

/* Serves until @p stop_sig (0: cwist_shutdown_request()) stops the server. */
static void serve_round(int stop_sig) {
    cwist_app *app = cwist_app_create();
    REQUIRE(app);
    cwist_app_get(app, "/ok", ok_handler);
    serve_args args = {app, pick_free_port(), -1};
    REQUIRE(args.port > 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, serve, &args) == 0);
    wait_until_up(args.port);

    /* While serving, CWIST owns the signals. */
    REQUIRE(!own_handlers_installed());
    if (stop_sig) {
        REQUIRE(raise(stop_sig) == 0);
    } else {
        cwist_shutdown_request();
    }
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(args.rc == 0);
    cwist_app_destroy(app);
    cwist_shutdown_reset();
}

static void test_signal_stops_the_server_then_handlers_come_back(void) {
    g_own_term = g_own_int = 0;
    serve_round(SIGTERM);
    /* CWIST handled that SIGTERM, not the process's handler. */
    REQUIRE(g_own_term == 0);
    REQUIRE(own_handlers_installed());

    /* After listen returned, both signals reach the process again and do
     * not count as a shutdown request. */
    REQUIRE(raise(SIGINT) == 0);
    REQUIRE(raise(SIGTERM) == 0);
    REQUIRE(g_own_int == 1 && g_own_term == 1);
    REQUIRE(atomic_load(&g_cwist_running) == 1);
    printf("Passed SIGTERM stops the server and the previous handlers come back\n");
}

static void test_every_round_restores(void) {
    g_own_term = g_own_int = 0;
    serve_round(0);
    REQUIRE(own_handlers_installed());
    serve_round(SIGINT);
    REQUIRE(own_handlers_installed());
    REQUIRE(g_own_int == 0 && g_own_term == 0);
    printf("Passed every listen restores the previous handlers\n");
}

static void test_install_and_restore_directly(void) {
    /* Restoring without an install changes nothing. */
    cwist_shutdown_restore_handlers();
    REQUIRE(own_handlers_installed());
    /* A second install does not overwrite the saved handlers with CWIST's. */
    cwist_shutdown_install_handlers();
    cwist_shutdown_install_handlers();
    REQUIRE(!own_handlers_installed());
    cwist_shutdown_restore_handlers();
    REQUIRE(own_handlers_installed());
    printf("Passed direct install and restore\n");
}

int main(void) {
    printf("Testing shutdown signal handlers around listen...\n");
    g_cwist_drain_timeout_sec = 0;
    install_own_handlers();
    test_signal_stops_the_server_then_handlers_come_back();
    test_every_round_restores();
    test_install_and_restore_directly();
    printf("All listen signal handler tests passed.\n");
    return 0;
}
