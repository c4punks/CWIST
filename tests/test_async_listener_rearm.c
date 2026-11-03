#define _POSIX_C_SOURCE 200809L
#define cwist_reactor_add test_reactor_add
#define cwist_reactor_timer_arm test_reactor_timer_arm
#define cwist_reactor_stop test_reactor_stop
#include "../src/net/http/async_server.c"
#undef cwist_reactor_add
#undef cwist_reactor_timer_arm
#undef cwist_reactor_stop

#include <assert.h>
#include <stdio.h>

static int add_calls;
static int timer_calls;
static int stop_calls;
static int add_sequence;

bool test_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, const void *payload,
                      size_t payload_size) {
    (void)reactor;
    (void)fd;
    (void)cb;
    (void)payload;
    (void)payload_size;
    add_calls++;
    return add_sequence++ >= 2;
}

bool test_reactor_timer_arm(cwist_reactor_t *reactor, cwist_reactor_timer_t *timer,
                            uint64_t delay_us) {
    (void)reactor;
    (void)timer;
    (void)delay_us;
    timer_calls++;
    return true;
}

void test_reactor_stop(cwist_reactor_t *reactor) {
    (void)reactor;
    stop_calls++;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reset_case(cwist_app *app) {
    add_calls = timer_calls = stop_calls = add_sequence = 0;
    g_reactor = (cwist_reactor_t *)0x1;
    g_accept_rearm_attempt = 0;
    g_accept_rearm_pending = false;
    g_accept_rearm_timer.cb = async_accept_rearm_cb;
    g_accept_rearm_timer.ctx = app;
    g_cwist_listen_fd = -1;
    atomic_store(&g_cwist_running, 1);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    g_cwist_listen_fd = fd;

    reset_case(app);
    uint64_t started = now_ns();
    async_accept_cb(fd, &app);
    assert(now_ns() - started < 100000000ull);
    assert(add_calls == 1 && timer_calls == 1 && stop_calls == 0);
    assert(g_accept_rearm_pending && g_accept_rearm_attempt == 1);

    g_accept_rearm_timer.cb(app);
    assert(add_calls == 2 && timer_calls == 2);
    assert(g_accept_rearm_pending && g_accept_rearm_attempt == 2);
    g_accept_rearm_timer.cb(app);
    assert(add_calls == 3 && timer_calls == 2);
    assert(!g_accept_rearm_pending && g_accept_rearm_attempt == 0);

    reset_case(app);
    async_accept_cb(fd, &app);
    assert(g_accept_rearm_pending);
    atomic_store(&g_cwist_running, 0);
    g_accept_rearm_timer.cb(app);
    assert(stop_calls == 1 && !g_accept_rearm_pending);
    assert(add_calls == 1);

    /* Multishot -> one-shot fallback (issue #523): a failed fallback re-arm
     * must enter the same bounded timer retry as the normal path instead of
     * silently dropping the listener. */
    reset_case(app);
    g_accept_ctx.app = app;
    g_accept_ctx.listen_fd = fd;
    async_accept_uring_cb(&g_accept_ctx, -1);
    assert(add_calls == 1 && timer_calls == 1 && stop_calls == 0);
    assert(g_accept_rearm_pending && g_accept_rearm_attempt == 1);

    g_accept_rearm_timer.cb(app);
    assert(add_calls == 2 && timer_calls == 2);
    assert(g_accept_rearm_pending && g_accept_rearm_attempt == 2);
    g_accept_rearm_timer.cb(app);
    assert(add_calls == 3 && !g_accept_rearm_pending);
    assert(g_accept_rearm_attempt == 0);

    /* Shutdown must not leave a pending fallback retry behind. */
    reset_case(app);
    async_accept_uring_cb(&g_accept_ctx, -1);
    assert(g_accept_rearm_pending);
    atomic_store(&g_cwist_running, 0);
    g_accept_rearm_timer.cb(app);
    assert(stop_calls == 1 && !g_accept_rearm_pending);

    close(fd);
    cwist_app_destroy(app);
    puts("test_async_listener_rearm: all passed");
    return 0;
}
