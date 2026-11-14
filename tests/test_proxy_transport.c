#define _POSIX_C_SOURCE 200809L

#include <cwist/net/proxy/proxy.h>
#include <cwist/sys/app/shutdown.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int upstream_tcp_fd;
static int upstream_udp_fd;
static uint16_t upstream_tcp_port;
static uint16_t upstream_udp_port;
static uint16_t proxy_tcp_port;
static uint16_t proxy_udp_port;
static int proxy_result;

static uint16_t socket_port(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

static int loopback_socket(int type, uint16_t *port) {
    int fd = socket(AF_INET, type, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    *port = socket_port(fd);
    return fd;
}

static uint16_t unused_port(int type) {
    uint16_t port;
    int fd = loopback_socket(type, &port);
    close(fd);
    return port;
}

static void write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void read_all(int fd, void *data, size_t len) {
    unsigned char *p = data;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void *tcp_upstream_run(void *arg) {
    (void)arg;
    static const char *requests[] = {"PING\r\n", "CONNECT {}\r\n", "version\r\n"};
    static const char *responses[] = {"+PONG\r\n", "INFO {\"server_id\":\"test\"}\r\n",
                                      "VERSION 1.6.0\r\n"};
    for (size_t i = 0; i < 3; i++) {
        int fd = accept(upstream_tcp_fd, NULL, NULL);
        assert(fd >= 0);
        char buf[128];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        assert(n == (ssize_t)strlen(requests[i]));
        assert(memcmp(buf, requests[i], (size_t)n) == 0);
        write_all(fd, responses[i], strlen(responses[i]));
        close(fd);
    }
    close(upstream_tcp_fd);
    return NULL;
}

static void *udp_upstream_run(void *arg) {
    (void)arg;
    unsigned char buf[256];
    for (int i = 0; i < 2; i++) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n =
            recvfrom(upstream_udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
        assert(n > 0);
        ssize_t sent = sendto(upstream_udp_fd, buf, (size_t)n, MSG_NOSIGNAL,
                              (struct sockaddr *)&peer, peer_len);
        assert(sent == n);
    }
    close(upstream_udp_fd);
    return NULL;
}

static void *proxy_run(void *arg) {
    (void)arg;
    proxy_result = cwist_proxy_listen();
    return NULL;
}

static int proxy_tcp_connect(void) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(proxy_tcp_port),
    };
    int fd = -1;
    for (int i = 0; i < 100; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        usleep(10000);
    }
    return -1;
}

static void tcp_exchange(const char *request, const char *response) {
    int fd = proxy_tcp_connect();
    assert(fd >= 0);
    struct timeval timeout = {.tv_sec = 2};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    write_all(fd, request, strlen(request));
    assert(shutdown(fd, SHUT_WR) == 0);
    char buf[128];
    size_t response_len = strlen(response);
    read_all(fd, buf, response_len);
    assert(memcmp(buf, response, response_len) == 0);
    assert(recv(fd, buf, 1, 0) == 0);
    close(fd);
}

static void udp_exchange(int fd, const struct sockaddr_in *proxy_addr, const unsigned char *request,
                         size_t request_len) {
    unsigned char response[256];
    ssize_t sent = sendto(fd, request, request_len, MSG_NOSIGNAL,
                          (const struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    assert(sent == (ssize_t)request_len);
    ssize_t n = recv(fd, response, sizeof(response), 0);
    assert(n == (ssize_t)request_len);
    assert(memcmp(response, request, request_len) == 0);
}

static void test_registration_validation(void) {
    cwist_proxy_reset();
    assert(cwist_proxy_tcp_forward(0, "127.0.0.1", 6379).error.err_i16 == -1);
    assert(cwist_proxy_tcp_forward(16379, NULL, 6379).error.err_i16 == -1);
    assert(cwist_proxy_tcp_forward(16379, "127.0.0.1", 0).error.err_i16 == -1);
    assert(cwist_proxy_udp_forward(0, "127.0.0.1", 53).error.err_i16 == -1);
    assert(cwist_proxy_udp_forward(10053, "", 53).error.err_i16 == -1);
    assert(cwist_proxy_tcp_forward(16379, "127.0.0.1", 6379).error.err_i16 == 0);
    assert(cwist_proxy_tcp_forward(16379, "127.0.0.1", 6379).error.err_i16 == -1);
    assert(cwist_proxy_udp_forward(16379, "127.0.0.1", 53).error.err_i16 == 0);
    cwist_proxy_reset();
}

int main(void) {
    test_registration_validation();

    upstream_tcp_fd = loopback_socket(SOCK_STREAM, &upstream_tcp_port);
    assert(listen(upstream_tcp_fd, 4) == 0);
    upstream_udp_fd = loopback_socket(SOCK_DGRAM, &upstream_udp_port);
    proxy_tcp_port = unused_port(SOCK_STREAM);
    proxy_udp_port = unused_port(SOCK_DGRAM);

    pthread_t tcp_upstream;
    pthread_t udp_upstream;
    assert(pthread_create(&tcp_upstream, NULL, tcp_upstream_run, NULL) == 0);
    assert(pthread_create(&udp_upstream, NULL, udp_upstream_run, NULL) == 0);

    assert(cwist_proxy_tcp_forward(proxy_tcp_port, "127.0.0.1", upstream_tcp_port).error.err_i16 ==
           0);
    assert(cwist_proxy_udp_forward(proxy_udp_port, "127.0.0.1", upstream_udp_port).error.err_i16 ==
           0);
    cwist_shutdown_reset();
    pthread_t proxy;
    assert(pthread_create(&proxy, NULL, proxy_run, NULL) == 0);

    tcp_exchange("PING\r\n", "+PONG\r\n");
    tcp_exchange("CONNECT {}\r\n", "INFO {\"server_id\":\"test\"}\r\n");
    tcp_exchange("version\r\n", "VERSION 1.6.0\r\n");

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp >= 0);
    struct timeval timeout = {.tv_sec = 2};
    assert(setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in proxy_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(proxy_udp_port),
    };
    const unsigned char dns_query[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char ntp_query[48] = {0x23};
    usleep(50000);
    udp_exchange(udp, &proxy_addr, dns_query, sizeof(dns_query));
    udp_exchange(udp, &proxy_addr, ntp_query, sizeof(ntp_query));
    close(udp);

    assert(pthread_join(tcp_upstream, NULL) == 0);
    assert(pthread_join(udp_upstream, NULL) == 0);
    cwist_shutdown_request();
    assert(pthread_join(proxy, NULL) == 0);
    assert(proxy_result == 0);
    cwist_proxy_reset();
    cwist_shutdown_reset();
    puts("test_proxy_transport: TCP Redis/NATS/Memcached and UDP DNS/NTP passed");
    return 0;
}
