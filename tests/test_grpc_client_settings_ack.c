#define _POSIX_C_SOURCE 200809L
/* gRPC client SETTINGS ACK validation tests (issue #720): a raw h2c server
 * completes the client preface handshake and answers the client SETTINGS with
 * a SETTINGS ACK that carries a 6-byte payload.  RFC 7540 section 6.5 makes
 * that a connection error of type FRAME_SIZE_ERROR, so
 * cwist_grpc_client_connect() must reject the handshake instead of treating
 * the ack as valid. */
#include <cwist/net/grpc/grpc_client.h>
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void read_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_frame(int fd, int type, int flags, uint32_t stream, const void *payload,
                       uint32_t len) {
    unsigned char h[9] = {
        (unsigned char)(len >> 16),    (unsigned char)(len >> 8),    (unsigned char)len,
        (unsigned char)type,           (unsigned char)flags,         (unsigned char)(stream >> 24),
        (unsigned char)(stream >> 16), (unsigned char)(stream >> 8), (unsigned char)stream};
    if (write_all(fd, h, sizeof(h)) != 0) return;
    if (len) (void)write_all(fd, payload, len);
}

static void read_frame(int fd, int *type) {
    unsigned char h[9];
    read_all(fd, h, sizeof(h));
    uint32_t len = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | h[2];
    *type = h[3];
    unsigned char buf[4096];
    while (len) {
        size_t take = len > sizeof(buf) ? sizeof(buf) : len;
        read_all(fd, buf, take);
        len -= (uint32_t)take;
    }
}

static void *serve(void *arg) {
    int listen_fd = *(int *)arg;
    int fd = accept(listen_fd, NULL, NULL);
    assert(fd >= 0);

    char preface[24];
    read_all(fd, preface, sizeof(preface));

    /* Client SETTINGS (non-ACK). */
    int type;
    do {
        read_frame(fd, &type);
    } while (type != 4);

    /* Server SETTINGS, then a SETTINGS ACK carrying a non-empty payload. */
    send_frame(fd, 4, 0, 0, NULL, 0);
    static const unsigned char ack_payload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x64};
    send_frame(fd, 4, 1, 0, ack_payload, sizeof(ack_payload));

    /* The client must drop the connection; drain whatever it sends last. */
    unsigned char discard[4096];
    while (recv(fd, discard, sizeof(discard), 0) > 0) {
    }
    close(fd);
    close(listen_fd);
    return NULL;
}

static uint16_t start_server(pthread_t *tid) {
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

    int *ctx = malloc(sizeof(*ctx));
    assert(ctx != NULL);
    *ctx = fd;
    assert(pthread_create(tid, NULL, serve, ctx) == 0);
    return ntohs(addr.sin_port);
}

int main(void) {
    pthread_t tid;
    uint16_t port = start_server(&tid);

    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client == NULL);

    pthread_join(tid, NULL);
    puts("test_grpc_client_settings_ack: all passed");
    return 0;
}
