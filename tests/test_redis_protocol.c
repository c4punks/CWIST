#define _POSIX_C_SOURCE 200809L
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail_append;
cwist_error_t __real_cwist_sstring_append(cwist_sstring *str, const char *data);
cwist_error_t __wrap_cwist_sstring_append(cwist_sstring *str, const char *data) {
    if (fail_append) return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_sstring_append(str, data);
}

typedef struct {
    int listen_fd;
    uint16_t port;
    const char *wire;
    size_t wire_len;
    pthread_t thread;
} mock_server;

static int send_all(int fd, const char *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, data + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static void *mock_main(void *arg) {
    mock_server *server = arg;
    int fd = accept(server->listen_fd, NULL, NULL);
    if (fd >= 0) {
        char request[4096];
        (void)recv(fd, request, sizeof(request), 0);
        (void)send_all(fd, server->wire, server->wire_len);
        close(fd);
    }
    close(server->listen_fd);
    return NULL;
}

static void mock_start(mock_server *server, const char *wire) {
    memset(server, 0, sizeof(*server));
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server->listen_fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_len) == 0);
    server->port = ntohs(addr.sin_port);
    assert(listen(server->listen_fd, 1) == 0);
    server->wire = wire;
    server->wire_len = strlen(wire);
    assert(pthread_create(&server->thread, NULL, mock_main, server) == 0);
}

static cwist_redis_t *mock_connect(mock_server *server) {
    cwist_redis_t *redis = cwist_redis_connect("127.0.0.1", server->port);
    assert(redis);
    return redis;
}

static void mock_join(mock_server *server) {
    assert(pthread_join(server->thread, NULL) == 0);
}

static void test_malformed_replies(void) {
    static const char *const replies[] = {
        ":9223372036854775808\r\n",
        "$9223372036854775808\r\n",
        "*9223372036854775808\r\n",
        "+OK\n",
        "+bad\rline\r\n",
    };
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++) {
        mock_server server;
        mock_start(&server, replies[i]);
        cwist_redis_t *redis = mock_connect(&server);
        char *out = NULL;
        cwist_error_t err = cwist_redis_get(redis, "key", &out);
        assert(err.error.err_i16 != 0 && out == NULL);
        cwist_redis_close(redis);
        mock_join(&server);
    }

    mock_server server;
    mock_start(&server, "+OK\r\n");
    cwist_redis_t *redis = mock_connect(&server);
    char *out = NULL;
    cwist_error_t err = cwist_redis_get(redis, "key", &out);
    assert(err.error.err_i16 == 0 && out && strcmp(out, "OK") == 0);
    cwist_free(out);
    cwist_redis_close(redis);
    mock_join(&server);
}

typedef struct {
    char messages[4][8];
    size_t count;
} subscription_capture;

static void capture_message(const char *channel, const char *message, void *arg) {
    subscription_capture *capture = arg;
    assert(channel && strcmp(channel, "x") == 0);
    assert(capture->count < 4);
    snprintf(capture->messages[capture->count], sizeof(capture->messages[0]), "%s", message);
    capture->count++;
}

static void test_pubsub_consumes_nested_extras(void) {
    static const char wire[] =
        "*1\r\n*3\r\n$9\r\nsubscribe\r\n$1\r\nx\r\n:1\r\n"
        "*4\r\n$7\r\nmessage\r\n$1\r\nx\r\n$3\r\none\r\n"
        "*3\r\n$7\r\nmessage\r\n$1\r\nx\r\n$4\r\nfake\r\n"
        "*3\r\n$7\r\nmessage\r\n$1\r\nx\r\n$3\r\ntwo\r\n";
    mock_server server;
    mock_start(&server, wire);
    cwist_redis_t *redis = mock_connect(&server);
    const char *channels[] = {"x", NULL};
    subscription_capture capture = {0};
    cwist_error_t err = cwist_redis_subscribe(redis, channels, capture_message, &capture);
    assert(err.error.err_i16 == 0);
    assert(capture.count == 2);
    assert(strcmp(capture.messages[0], "one") == 0);
    assert(strcmp(capture.messages[1], "two") == 0);
    cwist_redis_close(redis);
    mock_join(&server);
}

static void test_command_allocation_failure(void) {
    mock_server server;
    mock_start(&server, "+OK\r\n");
    cwist_redis_t *redis = mock_connect(&server);
    char *out = NULL;
    fail_append = 1;
    cwist_error_t err = cwist_redis_get(redis, "key", &out);
    fail_append = 0;
    assert(err.error.err_i16 != 0 && out == NULL);
    cwist_redis_close(redis);
    mock_join(&server);
}

int main(void) {
    test_command_allocation_failure();
    test_malformed_replies();
    test_pubsub_consumes_nested_extras();
    puts("strict RESP and pub/sub framing: ok");
    return 0;
}
