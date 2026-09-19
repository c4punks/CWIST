#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Regression test for issue #181: the C1M-mode WebSocket path must be
 * callback-shaped and reactor-driven.  A blocking receive on one connection
 * must never park the worker that other connections share.
 *
 * Technique: attach both ends of two socketpairs to one reactor running on a
 * pthread, drive the other ends from the main thread as a masked RFC 6455
 * client, and assert incremental parsing, fragmentation reassembly,
 * PING/PONG, CLOSE echo, and — the core regression — that a full message
 * exchange on connection B completes while connection A sits idle.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <cwist/net/websocket/websocket.h>
#include <cwist/net/websocket/websocket_async.h>
#include <cwist/sys/io/reactor.h>
#include "../src/net/websocket/ws_async_internal.h"

#define TIMEOUT_MS 5000

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

typedef struct {
    cwist_ws_opcode_t opcode;
    uint8_t payload[512];
    size_t len;
} recorded_msg_t;

static recorded_msg_t g_last;
static int g_messages_seen;

static void on_message(cwist_websocket_async *ws, cwist_ws_frame *frame, void *user_data) {
    (void)ws;
    (void)user_data;
    pthread_mutex_lock(&g_mu);
    g_last.opcode = frame->opcode;
    g_last.len = frame->payload_len;
    if (frame->payload_len > 0) {
        assert(frame->payload_len <= sizeof(g_last.payload));
        memcpy(g_last.payload, frame->payload, frame->payload_len);
    }
    g_messages_seen++;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    /* Same ownership contract as the blocking API. */
    cwist_websocket_frame_destroy(frame);
}

/* Wait until more than `seen_before` messages have been delivered.
 * Returns the new count. */
static int wait_for_seen(recorded_msg_t *out, int seen_before) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TIMEOUT_MS / 1000;

    pthread_mutex_lock(&g_mu);
    while (g_messages_seen <= seen_before) {
        assert(pthread_cond_timedwait(&g_cv, &g_mu, &deadline) == 0);
    }
    *out = g_last;
    int count = g_messages_seen;
    pthread_mutex_unlock(&g_mu);
    return count;
}

/* Wait for the next on_message delivery; returns the count before this one. */
static int wait_for_message(recorded_msg_t *out) {
    pthread_mutex_lock(&g_mu);
    int seen_before = g_messages_seen;
    pthread_mutex_unlock(&g_mu);
    return wait_for_seen(out, seen_before);
}

/* Build and write one masked client frame (payloads < 126 bytes here). */
static void write_masked(int fd, uint8_t head0, const uint8_t *payload, size_t len) {
    assert(len < 126);
    uint8_t frame[140];
    size_t pos = 0;
    frame[pos++] = head0;
    frame[pos++] = (uint8_t)(0x80 | len);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) frame[pos++] = payload[i] ^ mask[i % 4];
    /* MSG_NOSIGNAL: a frame can race a server-side reap (EOF observed in the
     * idle test), and SIGPIPE must not kill the test process. */
    assert(send(fd, frame, pos, MSG_NOSIGNAL) == (ssize_t)pos);
}

/* Read one server frame (never masked) with a poll timeout. */
static void read_server_frame(int fd, uint8_t *opcode, uint8_t *payload, size_t *out_len) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);

    uint8_t head[2];
    assert(read(fd, head, 2) == 2);
    *opcode = head[0] & 0x0F;
    assert((head[0] & 0x80) != 0);
    uint64_t len = head[1] & 0x7F;
    assert((head[1] & 0x80) == 0);
    if (len == 126) {
        uint8_t ext[2];
        assert(read(fd, ext, 2) == 2);
        len = ((uint64_t)ext[0] << 8) | ext[1];
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, payload + got, len - got);
        assert(n > 0);
        got += (size_t)n;
    }
    *out_len = (size_t)len;
}

static void *reactor_thread(void *arg) {
    cwist_reactor_run((cwist_reactor_t *)arg);
    return NULL;
}

static void stop_cb(void *ctx) {
    cwist_reactor_stop((cwist_reactor_t *)ctx);
}

/* Idle-timeout scenarios in a forked child: the knob is cached once per
 * process, so the short timeout must be set before the first ws attach in
 * that process — the same fork pattern as tests/test_reactor_drain_chunk.c. */
static void run_idle_timeout_child(void) {
    setenv("CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC", "2", 1);

    cwist_reactor_t *reactor = cwist_reactor_create();
    assert(reactor);
    pthread_t rt;
    assert(pthread_create(&rt, NULL, reactor_thread, reactor) == 0);

    /* Scenario 1: a totally silent connection is reaped by the periodic
     * sweep within roughly timeout + sweep interval (2s + 1s), well under
     * the 5s poll budget. */
    int idle_fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, idle_fd) == 0);
    assert(cwist_websocket_async_attach(idle_fd[0], reactor, on_message, NULL, NULL, 0));
    struct pollfd pfd = {.fd = idle_fd[1], .events = POLLIN};
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    uint8_t b;
    assert(read(idle_fd[1], &b, 1) == 0); /* EOF: the server closed the fd */
    close(idle_fd[1]);
    printf("7. idle connection reaped: ok\n");

    /* Scenario 2: activity resets the clock.  timeout=2s, sweep=1s.
     * t=0 frame A (delivered, last_active=0); t=1 frame B (gap 1s < 2s, so
     * it survives and last_active=1); t=2.5 frame C (gap 1.5s < 2s thanks to
     * the reset — a stale clock would measure 2.5s and reap it). */
    int active_fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, active_fd) == 0);
    assert(cwist_websocket_async_attach(active_fd[0], reactor, on_message, NULL, NULL, 0));
    recorded_msg_t msg;
    write_masked(active_fd[1], 0x81, (const uint8_t *)"A", 1);
    wait_for_message(&msg);
    sleep(1);
    write_masked(active_fd[1], 0x81, (const uint8_t *)"B", 1);
    wait_for_message(&msg);
    usleep(1500000);
    write_masked(active_fd[1], 0x81, (const uint8_t *)"C", 1);
    wait_for_message(&msg);
    assert(msg.opcode == CWIST_WS_FRAME_TEXT);
    assert(msg.len == 1 && msg.payload[0] == 'C');
    printf("8. activity resets the idle clock: ok\n");

    /* Clean shutdown: close the active connection so its async state and the
     * sweep context are freed before the reactor goes away (ASan counts
     * anything still allocated at reactor_destroy as a leak). */
    uint8_t code[2] = {0x03, 0xE8}; /* 1000 */
    write_masked(active_fd[1], 0x88, code, 2);
    uint8_t opcode, payload[8];
    size_t len;
    read_server_frame(active_fd[1], &opcode, payload, &len);
    assert(opcode == CWIST_WS_FRAME_CLOSE);
    pfd.fd = active_fd[1];
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    assert(read(active_fd[1], payload, sizeof(payload)) == 0); /* EOF */
    close(active_fd[1]);

    cwist_reactor_post_t stop_node = {.cb = stop_cb, .ctx = reactor};
    assert(cwist_reactor_post(reactor, &stop_node));
    assert(pthread_join(rt, NULL) == 0);
    cwist_reactor_destroy(reactor);
}

static void test_idle_timeout(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        run_idle_timeout_child();
        ssize_t w = write(fds[1], "0", 1);
        (void)w;
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    char result = '?';
    ssize_t r = read(fds[0], &result, 1);
    close(fds[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(r == 1 && result == '0');
    printf("idle timeout (CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC) test passed\n");
}

int main(void) {
    /* Forked first: the child sets the 2s idle timeout, which is cached
     * once per process, before any WebSocket state exists. */
    test_idle_timeout();

    cwist_reactor_t *reactor = cwist_reactor_create();
    assert(reactor);

    int conn_a[2], conn_b[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, conn_a) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, conn_b) == 0);

    assert(cwist_websocket_async_attach(conn_a[0], reactor, on_message, NULL, NULL, 0));
    assert(cwist_websocket_async_attach(conn_b[0], reactor, on_message, NULL, NULL, 0));

    pthread_t rt;
    assert(pthread_create(&rt, NULL, reactor_thread, reactor) == 0);

    recorded_msg_t msg;

    /* 1. A complete text message arrives with correct payload and opcode. */
    write_masked(conn_a[1], 0x81, (const uint8_t *)"hello", 5);
    wait_for_message(&msg);
    assert(msg.opcode == CWIST_WS_FRAME_TEXT);
    assert(msg.len == 5);
    assert(memcmp(msg.payload, "hello", 5) == 0);
    printf("1. complete text message: ok\n");

    /* 2. A fragmented message (FIN=0 text + FIN=1 continuation) is delivered
     *    as ONE reassembled frame with the original opcode. */
    int seen_before = g_messages_seen;
    write_masked(conn_a[1], 0x01, (const uint8_t *)"he", 2);
    usleep(50000); /* Let the first fragment arrive alone. */
    write_masked(conn_a[1], 0x80, (const uint8_t *)"llo", 3);
    int seen_after = wait_for_message(&msg);
    assert(seen_after == seen_before + 1); /* exactly one delivery */
    assert(msg.opcode == CWIST_WS_FRAME_TEXT);
    assert(msg.len == 5);
    assert(memcmp(msg.payload, "hello", 5) == 0);
    printf("2. fragmented reassembly: ok\n");

    /* 5. A valid frame split across two TCP writes (partial header) is
     *    delivered after the second write (incremental stash parsing). */
    uint8_t split_frame[2 + 4 + 12];
    size_t pos = 0;
    split_frame[pos++] = 0x81;
    split_frame[pos++] = 0x80 | 12;
    uint8_t mask[4] = {0x9A, 0x78, 0x56, 0x34};
    memcpy(split_frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < 12; i++)
        split_frame[pos++] = ((const uint8_t *)"split-header")[i] ^ mask[i % 4];
    assert(write(conn_a[1], split_frame, 3) == 3);
    usleep(50000);
    assert(write(conn_a[1], split_frame + 3, pos - 3) == (ssize_t)(pos - 3));
    wait_for_message(&msg);
    assert(msg.opcode == CWIST_WS_FRAME_TEXT);
    assert(msg.len == 12);
    assert(memcmp(msg.payload, "split-header", 12) == 0);
    printf("5. split-frame incremental parse: ok\n");

    /* 3. A PING elicits a PONG with the identical payload. */
    int before_ping = g_messages_seen;
    write_masked(conn_a[1], 0x89, (const uint8_t *)"pingdata", 8);
    uint8_t opcode, payload[64];
    size_t len;
    read_server_frame(conn_a[1], &opcode, payload, &len);
    assert(opcode == CWIST_WS_FRAME_PONG);
    assert(len == 8);
    assert(memcmp(payload, "pingdata", 8) == 0);
    /* The async path also DELIVERS the PING frame to on_message (same
     * contract as the blocking API) in addition to queueing the PONG.
     * That delivery can land on either side of the PONG read above, so
     * wait against the count taken before the ping; otherwise this PING
     * would be the "next" message a later wait_for_message sees. */
    wait_for_seen(&msg, before_ping);
    assert(msg.opcode == CWIST_WS_FRAME_PING);
    assert(msg.len == 8);
    assert(memcmp(msg.payload, "pingdata", 8) == 0);
    printf("3. ping -> pong echo: ok\n");

    /* 6. Core regression: while connection A sits idle, a full message
     *    exchange on connection B completes — the reactor thread was never
     *    parked by A. */
    write_masked(conn_b[1], 0x81, (const uint8_t *)"bravo", 5);
    wait_for_message(&msg);
    assert(msg.opcode == CWIST_WS_FRAME_TEXT);
    assert(msg.len == 5);
    assert(memcmp(msg.payload, "bravo", 5) == 0);
    printf("6. second connection progresses while first idle: ok\n");

    /* 4. A client CLOSE is echoed with the status code, then the connection
     *    is closed. */
    uint8_t code[2] = {0x03, 0xE8}; /* 1000 */
    write_masked(conn_a[1], 0x88, code, 2);
    read_server_frame(conn_a[1], &opcode, payload, &len);
    assert(opcode == CWIST_WS_FRAME_CLOSE);
    assert(len == 2);
    assert(payload[0] == code[0] && payload[1] == code[1]);
    struct pollfd pfd = {.fd = conn_a[1], .events = POLLIN};
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    assert(read(conn_a[1], payload, sizeof(payload)) == 0); /* EOF */
    printf("4. close echo + connection closed: ok\n");

    /* Connection B is still attached; close it cleanly so its async state
     * (stash buffers) is freed before the reactor goes away. ASan counts
     * anything still allocated at reactor_destroy as a leak. */
    write_masked(conn_b[1], 0x88, code, 2);
    read_server_frame(conn_b[1], &opcode, payload, &len);
    assert(opcode == CWIST_WS_FRAME_CLOSE);
    pfd.fd = conn_b[1];
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    assert(read(conn_b[1], payload, sizeof(payload)) == 0); /* EOF */

    /* Stop the reactor thread via a posted callback (wakes the poller). */
    cwist_reactor_post_t stop_node = {.cb = stop_cb, .ctx = reactor};
    assert(cwist_reactor_post(reactor, &stop_node));
    assert(pthread_join(rt, NULL) == 0);

    cwist_reactor_destroy(reactor);
    close(conn_a[1]);
    close(conn_b[1]);
    /* conn_a[0]/conn_b[0] were owned and closed by the WS async layer when
     * each connection terminated; closing them here again would be a double
     * close on a possibly-reused fd. */

    printf("websocket async (C1M) test passed\n");
    return 0;
}
