#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Multishot-accept contract test (issue #406). Same approach as
 * test_reactor_wake.c: include the implementation so the accept slot and
 * ring internals are inspectable, replacing only the allocator and the
 * process shutdown flag.
 *
 * "./test_reactor_accept"      functional accept run (needs the uring ring)
 * "./test_reactor_accept off"  CWIST_ACCEPT_URING=0 must refuse the arm
 * CWIST_REACTOR_BACKEND=epoll  the arm must refuse on the epoll fallback
 *
 * CWIST_TEST_REQUIRE_IO_URING=1 fails rather than skipping a denied backend.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) {
    return calloc(1, size);
}
void cwist_free(void *ptr) {
    free(ptr);
}
atomic_int g_cwist_running = 1;

enum { TARGET = 64 };
static cwist_reactor_t *loop;
static pthread_t run_tid;
static _Atomic int accepted;
static _Atomic int terminals;
static _Atomic int last_terminal_err;

static void *run_thread(void *arg) {
    (void)arg;
    run_tid = pthread_self();
    cwist_reactor_run(loop);
    return NULL;
}

static void accept_cb(void *ctx, int fd) {
    (void)ctx;
    assert(pthread_equal(pthread_self(), run_tid));
    if (fd < 0) {
        atomic_fetch_add(&terminals, 1);
        atomic_store(&last_terminal_err, fd);
        return;
    }
    /* accept_flags must have produced a non-blocking socket. */
    int fl = fcntl(fd, F_GETFL, 0);
    assert(fl >= 0 && (fl & O_NONBLOCK));
    close(fd);
    if (atomic_fetch_add(&accepted, 1) + 1 == TARGET) cwist_reactor_stop(loop);
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 128) == 0);
    int fl = fcntl(fd, F_GETFL, 0);
    assert(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
    return fd;
}

static int listener_port(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

static void connect_n(int port, int fds[], int n) {
    for (int i = 0; i < n; i++) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        assert(c >= 0);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)port),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        assert(connect(c, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        fds[i] = c;
    }
}

/* On a real ring with the knob on, one multishot arm accepts TARGET
 * connections without a single re-arm. */
static void check_functional(void) {
    int lfd = make_listener();
    assert(cwist_reactor_accept_arm(loop, lfd, accept_cb, NULL));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_thread, NULL) == 0);
    int clients[TARGET];
    connect_n(listener_port(lfd), clients, TARGET);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&accepted) == TARGET);
    assert(atomic_load(&terminals) == 0);
    /* The multishot registration outlives the run: destroy must free it. */
    cwist_reactor_destroy(loop);
    loop = NULL;
    for (int i = 0; i < TARGET; i++) close(clients[i]);
    close(lfd);
}

/* An accept armed on a socket that is not listening completes with a
 * terminal error, exactly once, so the caller can fall back to the
 * one-shot POLL loop. */
static void check_terminal(void) {
    loop = cwist_reactor_create();
    assert(loop != NULL);
    if (loop->impl.use_epoll) {
        cwist_reactor_destroy(loop);
        loop = NULL;
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0); /* never listen()ed: accept must fail */
    assert(cwist_reactor_accept_arm(loop, fd, accept_cb, NULL));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_thread, NULL) == 0);
    for (int i = 0; i < 2000 && atomic_load(&terminals) == 0; i++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    assert(atomic_load(&terminals) == 1);
    assert(atomic_load(&last_terminal_err) < 0);
    cwist_reactor_stop(loop);
    assert(pthread_join(thread, NULL) == 0);
    cwist_reactor_destroy(loop);
    loop = NULL;
    close(fd);
}

int main(int argc, char **argv) {
    alarm(30); /* Watchdog only, not the correctness oracle. */
    bool knob_off = argc == 2 && strcmp(argv[1], "off") == 0;
    if (knob_off) setenv("CWIST_ACCEPT_URING", "0", 1);

    loop = cwist_reactor_create();
    assert(loop != NULL);
#ifdef __linux__
    printf("backend=%s\n", loop->impl.use_epoll ? "epoll" : "io_uring");
    if (loop->impl.use_epoll && getenv("CWIST_TEST_REQUIRE_IO_URING")) {
        fprintf(stderr, "required io_uring backend unavailable\n");
        cwist_reactor_destroy(loop);
        exit(1);
    }
    if (loop->impl.use_epoll) {
        /* Epoll fallback reactor: the multishot arm must refuse, leaving the
         * one-shot POLL accept path in charge. */
        int lfd = make_listener();
        assert(!cwist_reactor_accept_arm(loop, lfd, accept_cb, NULL));
        close(lfd);
        cwist_reactor_destroy(loop);
        puts("multishot accept refuses the epoll backend: PASS");
        return 0;
    }
    if (knob_off) {
        int lfd = make_listener();
        assert(!cwist_reactor_accept_arm(loop, lfd, accept_cb, NULL));
        close(lfd);
        cwist_reactor_destroy(loop);
        puts("CWIST_ACCEPT_URING=0 refuses the arm: PASS");
        return 0;
    }
    check_functional();
    check_terminal();
    puts("multishot accept, terminal error, slot teardown: PASS");
    return 0;
#else
    assert(!cwist_reactor_accept_arm(loop, -1, accept_cb, NULL));
    cwist_reactor_destroy(loop);
    puts("multishot accept is a no-op off Linux: PASS");
    return 0;
#endif
}
