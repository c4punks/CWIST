/** @file bench_webrtc.c
 * @brief WebRTC DataChannel micro-benchmark over UDP loopback.
 *
 * Measures three things between an answering ICE-lite ctx and an in-process
 * offering peer:
 *   1. idle cost: context switches and CPU time of a ctx with no traffic,
 *   2. one-way throughput for small and large messages,
 *   3. round-trip latency (p50/p99) of sequential ping/pong.
 *
 * Usage: ./bench_webrtc [small_count] [large_count]
 */
#include <cwist/net/webrtc.h>

#include "../src/net/webrtc/webrtc_internal.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    atomic_long msgs;
    atomic_long bytes;
    atomic_int echo;
} bench_server;

typedef struct {
    atomic_long pongs;
} bench_client;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void server_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, cwist_webrtc_data_type type, void *user) {
    (void)type;
    bench_server *s = user;
    atomic_fetch_add(&s->msgs, 1);
    atomic_fetch_add(&s->bytes, (long)len);
    if (atomic_load(&s->echo))
        cwist_webrtc_conn_send(conn, channel, data, len, CWIST_WEBRTC_DATA_BINARY);
}

static void client_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, cwist_webrtc_data_type type, void *user) {
    (void)type;
    (void)conn;
    (void)channel;
    (void)data;
    (void)len;
    bench_client *c = user;
    atomic_fetch_add(&c->pongs, 1);
}

/* Retry while the send queue pushes back. */
static void send_blocking(cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    while (cwist_webrtc_conn_send(conn, 1, data, len, CWIST_WEBRTC_DATA_BINARY) < 0) usleep(50);
}

/* Spin (yielding) rather than sleep: usleep() granularity would otherwise
 * dominate the RTT numbers. */
static int wait_until(atomic_long *v, long target, int timeout_ms) {
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (atomic_load(v) < target) {
        if (now_ns() > deadline) return -1;
        sched_yield();
    }
    return 0;
}

static void bench_throughput(cwist_webrtc_conn *conn, bench_server *s, long count, size_t size) {
    uint8_t *buf = malloc(size);
    memset(buf, 0x5A, size);
    long base = atomic_load(&s->msgs);
    uint64_t t0 = now_ns();
    for (long i = 0; i < count; i++) send_blocking(conn, buf, size);
    int rc = wait_until(&s->msgs, base + count, 60000);
    double sec = (double)(now_ns() - t0) / 1e9;
    long got = atomic_load(&s->msgs) - base;
    printf("throughput %7zu B x %-7ld: %s%.0f msg/s, %.1f MB/s (%.2fs)\n", size, count,
           rc ? "TIMEOUT " : "", (double)got / sec, (double)got * (double)size / sec / 1e6, sec);
    free(buf);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void bench_latency(cwist_webrtc_conn *conn, bench_server *s, bench_client *c, int rounds) {
    atomic_store(&s->echo, 1);
    uint64_t *rtt = malloc(sizeof(*rtt) * (size_t)rounds);
    int done = 0;
    for (int i = 0; i < rounds; i++) {
        long base = atomic_load(&c->pongs);
        uint64_t t0 = now_ns();
        send_blocking(conn, (const uint8_t *)"ping", 4);
        if (wait_until(&c->pongs, base + 1, 5000) < 0) break;
        rtt[done++] = now_ns() - t0;
    }
    atomic_store(&s->echo, 0);
    if (done) {
        qsort(rtt, (size_t)done, sizeof(*rtt), cmp_u64);
        printf("latency    rtt x %d: p50 %.1f us, p99 %.1f us, max %.1f us\n", done,
               (double)rtt[done / 2] / 1e3, (double)rtt[(size_t)done * 99 / 100] / 1e3,
               (double)rtt[done - 1] / 1e3);
    }
    free(rtt);
}

static void bench_idle(void) {
    cwist_webrtc_ctx *idle = cwist_webrtc_ctx_new(0);
    assert(idle);
    usleep(100000);
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    usleep(1000000);
    getrusage(RUSAGE_SELF, &r1);
    long csw = (r1.ru_nvcsw - r0.ru_nvcsw) + (r1.ru_nivcsw - r0.ru_nivcsw);
    double cpu_ms = (double)(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1e3 +
                    (double)(r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e3 +
                    (double)(r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1e3 +
                    (double)(r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e3;
    printf("idle ctx   1s           : %ld context switches, %.2f ms CPU\n", csw, cpu_ms);
    cwist_webrtc_ctx_free(idle);
}

int main(int argc, char **argv) {
    long small_count = argc > 1 ? atol(argv[1]) : 200000;
    long large_count = argc > 2 ? atol(argv[2]) : 2000;

    bench_idle();

    bench_server s = {0};
    bench_client c = {0};
    cwist_webrtc_ctx *server = cwist_webrtc_ctx_new(0);
    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(server && client);
    cwist_webrtc_ctx_set_message_handler(server, &server_on_message, &s);
    cwist_webrtc_ctx_set_message_handler(client, &client_on_message, &c);

    char offer[1024], answer[2048];
    assert(cwist_sdp_write_offer(offer, sizeof(offer),
                                 "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:"
                                 "44:55:66:77:88:99:AA:BB:CC:DD:EE:FF",
                                 "benchfrag", "benchpassword0123456789012", "0") == 0);
    assert(cwist_webrtc_handle_offer(server, offer, answer, sizeof(answer)) == 0);
    cwist_sdp_info ans;
    assert(cwist_sdp_parse(answer, strlen(answer), &ans) == 0);

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(cwist_webrtc_ctx_port(server));
    remote.sin_addr.s_addr = htonl(0x7F000001);
    cwist_webrtc_conn *conn = cwist_webrtc_connect(client, &remote, ans.ice_ufrag, ans.ice_pwd);
    assert(conn);

    /* Warm-up: open the channel and wait for the first message to land. */
    send_blocking(conn, (const uint8_t *)"warm", 4);
    if (wait_until(&s.msgs, 1, 10000) < 0) {
        fprintf(stderr, "bench_webrtc: connection did not come up\n");
        return 1;
    }

    bench_throughput(conn, &s, small_count, 64);
    bench_throughput(conn, &s, small_count / 4, 1024);
    bench_throughput(conn, &s, large_count, 65536);
    bench_latency(conn, &s, &c, 2000);

    cwist_webrtc_ctx_free(client);
    cwist_webrtc_ctx_free(server);
    return 0;
}
