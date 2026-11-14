#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc_client.h>
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    int over_limit;
    int push;
} server_ctx;

static void read_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void send_frame(int fd, int type, int flags, uint32_t stream, const void *payload,
                       uint32_t len) {
    unsigned char h[9] = {
        (unsigned char)(len >> 16),    (unsigned char)(len >> 8),    (unsigned char)len,
        (unsigned char)type,           (unsigned char)flags,         (unsigned char)(stream >> 24),
        (unsigned char)(stream >> 16), (unsigned char)(stream >> 8), (unsigned char)stream};
    write_all(fd, h, sizeof(h));
    if (len) write_all(fd, payload, len);
}

static void read_frame(int fd, int *type, uint32_t *stream) {
    unsigned char h[9];
    read_all(fd, h, sizeof(h));
    uint32_t len = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | h[2];
    *type = h[3];
    *stream =
        ((uint32_t)(h[5] & 0x7f) << 24) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 8) | h[8];
    unsigned char buf[4096];
    while (len) {
        size_t take = len > sizeof(buf) ? sizeof(buf) : len;
        read_all(fd, buf, take);
        len -= (uint32_t)take;
    }
}

static void *serve(void *arg) {
    server_ctx *ctx = arg;
    int fd = accept(ctx->fd, NULL, NULL);
    assert(fd >= 0);
    char preface[24];
    read_all(fd, preface, sizeof(preface));
    int type;
    uint32_t stream;
    do {
        read_frame(fd, &type, &stream);
    } while (type != 4);
    send_frame(fd, 4, 0, 0, NULL, 0);
    send_frame(fd, 4, 1, 0, NULL, 0);
    do {
        read_frame(fd, &type, &stream);
    } while (type != 0);

    static const unsigned char status_ok[] = {0x88};
    if (ctx->push == 1) {
        static const unsigned char other_stream_headers[] = {0x40, 1, 'x', 1, 'y'};
        send_frame(fd, 1, 4, 2, other_stream_headers, sizeof(other_stream_headers));
    } else if (ctx->push == 2) {
        static const unsigned char push_promise[] = {0, 0, 0, 4};
        send_frame(fd, 5, 4, stream, push_promise, sizeof(push_promise));
    }
    if (ctx->over_limit) {
        send_frame(fd, 1, 0, stream, status_ok, sizeof(status_ok));
        unsigned char zeros[4096] = {0};
        for (int i = 0; i < 16; i++) send_frame(fd, 9, 0, stream, zeros, sizeof(zeros));
        send_frame(fd, 9, 4, stream, zeros, 1);
    } else {
        send_frame(fd, 1, 0, stream, NULL, 0);
        send_frame(fd, 9, 4, stream, status_ok, sizeof(status_ok));
        const unsigned char data[] = {0, 0, 0, 0, 1, 'x'};
        send_frame(fd, 0, 1, stream, data, sizeof(data));
    }
    struct timespec delay = {0, 100000000L};
    nanosleep(&delay, NULL);
    close(fd);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

static uint16_t start_server(int over_limit, int push, pthread_t *tid) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 1) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    server_ctx *ctx = malloc(sizeof(*ctx));
    assert(ctx != NULL);
    ctx->fd = fd;
    ctx->over_limit = over_limit;
    ctx->push = push;
    assert(pthread_create(tid, NULL, serve, ctx) == 0);
    return ntohs(addr.sin_port);
}

static void run_case(int over_limit, int push) {
    pthread_t tid;
    uint16_t port = start_server(over_limit, push, &tid);
    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client != NULL);
    cwist_grpc_call *call = cwist_grpc_call_start(client, "/test.Test/Call", NULL, 0, 2000);
    assert(call != NULL);
    cwist_grpc_message msg;
    int rc = cwist_grpc_call_recv(call, &msg);
    if (over_limit || push) {
        assert(rc != 1);
        if (push) assert(cwist_grpc_client_dead(client));
    } else {
        assert(rc == 1 && msg.len == 1 && msg.data[0] == 'x');
    }
    cwist_grpc_call_destroy(call);
    cwist_grpc_client_close(client);
    pthread_join(tid, NULL);
}

int main(void) {
    run_case(0, 0);
    run_case(1, 0);
    run_case(0, 1);
    run_case(0, 2);
    puts("test_grpc_client_headers: all passed");
    return 0;
}
