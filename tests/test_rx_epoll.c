#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* RX-epoll receive path (reactor_rx.h) on a forced epoll reactor.
 *
 * Includes the implementation, like test_reactor_wake.c, so the test can
 * count epoll_ctl calls and check that a steady re-arm makes none. Every
 * step runs on the reactor's run thread (timers and callbacks), with the
 * peer end of a socketpair written from inside those callbacks:
 *
 *   1. off-thread arms are refused (the POLL fallback covers them)
 *   2. a keep-alive loop: one registration, then no epoll_ctl per re-arm
 *   3. a buffer smaller than the queued bytes: the rest arrives without a
 *      new edge
 *   4. bytes that arrive while no receive is armed reach the next arm
 *   5. a one-shot cwist_reactor_add takes the fd over, and the next RX arm
 *      registers it again
 *   6. a different connection pointer on the same fd registers again
 *   7. peer close reports 0 (EOF)
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>

static int g_epoll_ctl_calls;
static int counted_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    g_epoll_ctl_calls++;
    return epoll_ctl(epfd, op, fd, ev);
}
#define epoll_ctl counted_epoll_ctl
#include "../src/sys/io/reactor.c"
#undef epoll_ctl

void *cwist_alloc(size_t size) {
    return calloc(1, size);
}
void cwist_free(void *ptr) {
    free(ptr);
}
atomic_int g_cwist_running = 1;

enum { KEEPALIVE_ROUNDS = 200, BIG = 100, SMALL = 10 };

static cwist_reactor_t *loop;
static int fd_a, fd_b; /* reactor side, peer side */
static char buf[256];
static int conn1, conn2; /* addresses only: the connection pointers */
static int phase;
static int rounds;
static size_t got;
static int ctl_mark;
static cwist_reactor_timer_t timer;

static void peer_write(const char *s, size_t n) {
    assert(write(fd_b, s, n) == (ssize_t)n);
}

static void arm(void *conn, unsigned len) {
    assert(cwist_reactor_recv_arm(loop, fd_a, buf, len, conn, NULL));
}

static void step(void *ctx);

static void oneshot_cb(int fd, void *ctx) {
    (void)ctx;
    char c;
    assert(phase == 5);
    assert(recv(fd, &c, 1, 0) == 1 && c == 'x');
    /* Back to RX: the one-shot registration replaced the edge-triggered one,
     * so this arm must register again. */
    ctl_mark = g_epoll_ctl_calls;
    arm(&conn1, sizeof(buf));
    assert(g_epoll_ctl_calls == ctl_mark + 1);
    peer_write("y", 1);
}

static void rx_cb(void *conn, int res) {
    switch (phase) {
        case 2: /* keep-alive loop */
            assert(conn == &conn1);
            assert(res == 5);
            char want[8];
            snprintf(want, sizeof(want), "p%04d", rounds);
            assert(memcmp(buf, want, 5) == 0);
            if (++rounds < KEEPALIVE_ROUNDS) {
                arm(&conn1, sizeof(buf));
                snprintf(want, sizeof(want), "p%04d", rounds);
                peer_write(want, 5);
                return;
            }
            assert(g_epoll_ctl_calls == ctl_mark);
            printf("Passed %d keep-alive receives with no epoll_ctl after the first arm\n",
                   KEEPALIVE_ROUNDS);
            phase = 3;
            got = 0;
            {
                char big[BIG];
                for (int i = 0; i < BIG; i++) big[i] = (char)('a' + i % 26);
                peer_write(big, BIG);
            }
            arm(&conn1, SMALL);
            return;
        case 3: /* small buffer, no new edges */
            assert(res == SMALL);
            for (int i = 0; i < SMALL; i++) assert(buf[i] == (char)('a' + (got + (size_t)i) % 26));
            got += SMALL;
            if (got < BIG) {
                arm(&conn1, SMALL);
                return;
            }
            printf("Passed %d bytes through a %d-byte buffer without new edges\n", BIG, SMALL);
        /* Leave it unarmed and send more: the edge lands while unarmed. */
            phase = 4;
            peer_write("late", 4);
            cwist_reactor_timer_arm(loop, &timer, 20 * 1000);
            return;
        case 4:
            assert(res == 4 && memcmp(buf, "late", 4) == 0);
            printf("Passed bytes that arrived while unarmed reach the next arm\n");
            phase = 5;
        /* Unarmed again: hand the fd to the one-shot path. */
            {
                int dummy = 0;
                assert(cwist_reactor_add(loop, fd_a, oneshot_cb, &dummy, sizeof(dummy)));
            }
            peer_write("x", 1);
            return;
        case 5:
            assert(conn == &conn1 && res == 1 && buf[0] == 'y');
            printf("Passed one-shot add takes the fd over and RX re-registers after\n");
            phase = 6;
            ctl_mark = g_epoll_ctl_calls;
            arm(&conn2, sizeof(buf));
            assert(g_epoll_ctl_calls == ctl_mark + 1);
            peer_write("z", 1);
            return;
        case 6:
            assert(conn == &conn2 && res == 1 && buf[0] == 'z');
            printf("Passed a new connection pointer on the same fd registers again\n");
            phase = 7;
            arm(&conn2, sizeof(buf));
            close(fd_b);
            return;
        case 7:
            assert(conn == &conn2 && res == 0);
            printf("Passed peer close reports EOF\n");
            phase = 8;
            cwist_reactor_stop(loop);
            return;
        default: assert(!"unexpected receive");
    }
}

static void step(void *ctx) {
    (void)ctx;
    switch (phase) {
        case 0:
        /* First arm registers the fd once. */
            phase = 2;
            rounds = 0;
            g_epoll_ctl_calls = 0;
            arm(&conn1, sizeof(buf));
            assert(g_epoll_ctl_calls == 1);
            ctl_mark = g_epoll_ctl_calls;
            peer_write("p0000", 5);
            return;
        case 4:
        /* The 20 ms timer: by now the reactor saw the edge while unarmed. */
            arm(&conn1, sizeof(buf));
            return;
        default: assert(!"unexpected timer");
    }
}

int main(void) {
    setenv("CWIST_REACTOR_BACKEND", "epoll", 1);
    loop = cwist_reactor_create();
    assert(loop && loop->impl.use_epoll);
    assert(cwist_reactor_rx_supported(loop));
    cwist_reactor_set_rx_cb(loop, rx_cb);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    fd_a = sv[0];
    fd_b = sv[1];

    /* 1. Not on the run thread: refused. */
    assert(!cwist_reactor_recv_arm(loop, fd_a, buf, sizeof(buf), &conn1, NULL));
    printf("Passed off-thread arm is refused\n");

    cwist_reactor_timer_init(&timer, step, NULL);
    assert(cwist_reactor_timer_arm(loop, &timer, 0));
    cwist_reactor_run(loop);
    assert(phase == 8);

    /* fd_forget resets the registration hint. */
    assert(epfd_get_state(fd_a) == EPFD_RX);
    cwist_reactor_fd_forget(fd_a);
    assert(epfd_get_state(fd_a) == EPFD_UNKNOWN);
    printf("Passed fd_forget resets the registration state\n");

    close(fd_a);
    cwist_reactor_destroy(loop);
    printf("All RX-epoll tests passed.\n");
    return 0;
}
