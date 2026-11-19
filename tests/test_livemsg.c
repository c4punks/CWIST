#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/livemsg.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/websocket/websocket.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 30000

static int raw_port;
static int json_port;
static bool raw_backpressure;

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static uint16_t unused_tcp_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int connect_retry(uint16_t port) {
    for (int attempt = 0; attempt < 250; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_port = htons(port),
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        sleep_ms(20);
    }
    assert(!"server did not start");
    return -1;
}

static void read_exact(int fd, void *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
        ssize_t n = recv(fd, (char *)data + done, len - done, 0);
        assert(n > 0);
        done += (size_t)n;
    }
}

static int ws_connect(uint16_t port, const char *path) {
    int fd = connect_retry(port);
    char request[512];
    int len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Sec-WebSocket-Version: 13\r\n\r\n",
                       path);
    assert(send(fd, request, (size_t)len, MSG_NOSIGNAL) == len);
    char response[2048];
    size_t used = 0;
    while (used < sizeof(response) - 1) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
        ssize_t n = recv(fd, response + used, 1, 0);
        assert(n > 0);
        used += (size_t)n;
        response[used] = 0;
        if (strstr(response, "\r\n\r\n")) break;
    }
    assert(strstr(response, "HTTP/1.1 101") != NULL);
    return fd;
}

static void send_all(int fd, const void *data, size_t len) {
    const uint8_t *bytes = data;
    while (len) {
        ssize_t n = send(fd, bytes, len, MSG_NOSIGNAL);
        if (n > 0) {
            bytes += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) return;
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    }
}

static void ws_write(int fd, uint8_t opcode, const void *payload, size_t len) {
    size_t header_len = len < 126 ? 6 : len <= 65535 ? 8 : 14;
    uint8_t header[14];
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | (len < 126 ? len : len <= 65535 ? 126 : 127);
    size_t pos = 2;
    if (len >= 126) {
        size_t ext = len <= 65535 ? 2 : 8;
        for (size_t i = 0; i < ext; i++) header[pos++] = (uint8_t)(len >> ((ext - i - 1) * 8));
    }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(header + pos, mask, 4);
    pos += 4;
    assert(pos == header_len);
    send_all(fd, header, header_len);
    const uint8_t *bytes = payload;
    uint8_t block[4096];
    while (len) {
        size_t chunk = len < sizeof(block) ? len : sizeof(block);
        for (size_t i = 0; i < chunk; i++) block[i] = bytes[i] ^ mask[i % 4];
        send_all(fd, block, chunk);
        bytes += chunk;
        len -= chunk;
    }
}

typedef struct {
    uint8_t opcode;
    uint8_t *payload;
    size_t len;
} client_frame;

static client_frame ws_read(int fd) {
    uint8_t head[2];
    read_exact(fd, head, 2);
    client_frame frame = {.opcode = head[0] & 0x0f};
    assert((head[0] & 0x80) && !(head[1] & 0x80));
    uint64_t len = head[1] & 0x7f;
    if (len == 126) {
        uint8_t ext[2];
        read_exact(fd, ext, 2);
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        read_exact(fd, ext, 8);
        len = 0;
        for (size_t i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    assert(len <= CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
    frame.payload = malloc((size_t)len + 1);
    assert(frame.payload);
    read_exact(fd, frame.payload, (size_t)len);
    frame.payload[len] = 0;
    frame.len = (size_t)len;
    return frame;
}

static void frame_free(client_frame *frame) {
    free(frame->payload);
    frame->payload = NULL;
}

static void expect_frame(int fd, const void *data, size_t len) {
    client_frame frame = ws_read(fd);
    assert(frame.len == len);
    assert(!len || memcmp(frame.payload, data, len) == 0);
    frame_free(&frame);
}

static void broadcast_one(cwist_livemsg_peer *peer, void *arg) {
    (void)arg;
    assert(cwist_livemsg_send_raw(peer, "broadcast", 9) == 0);
}

static int raw_process(cwist_livemsg_peer *peer, const void *msg, size_t len, void **result,
                       void *user_data) {
    (void)user_data;
    if (len == 7 && !memcmp(msg, "noreply", 7)) return CWIST_LIVEMSG_NO_REPLY;
    if (len == 9 && !memcmp(msg, "broadcast", 9)) {
        cwist_livemsg_foreach_peer(cwist_livemsg_peer_ctx(peer), broadcast_one, NULL);
        return CWIST_LIVEMSG_NO_REPLY;
    }
    char *copy = cwist_livemsg_alloc(peer, len + 1);
    assert(copy);
    memcpy(copy, msg, len);
    copy[len] = 0;
    *result = copy;
    return CWIST_LIVEMSG_OK;
}

static int raw_respond(cwist_livemsg_peer *peer, void *result, cwist_livemsg_reply *reply,
                       void *user_data) {
    (void)user_data;
    const char *msg = result;
    if (!strcmp(msg, "fill")) {
        uint8_t *payload = calloc(1, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
        assert(payload);
        raw_backpressure = false;
        for (int i = 0; i < 64; i++) {
            int rc = cwist_livemsg_send_raw(peer, payload, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
            if (rc == -EAGAIN) {
                raw_backpressure = true;
                break;
            }
            assert(rc == 0);
        }
        free(payload);
        return CWIST_LIVEMSG_NO_REPLY;
    }
    if (!strcmp(msg, "check-full"))
        return cwist_livemsg_reply_write(reply, raw_backpressure ? "full" : "not-full",
                                         raw_backpressure ? 4 : 8);
    if (!strcmp(msg, "max")) {
        uint8_t *payload = calloc(1, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
        assert(payload);
        int rc = cwist_livemsg_reply_write(reply, payload, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
        free(payload);
        return rc;
    }
    if (!strcmp(msg, "too-big")) {
        uint8_t *payload = calloc(1, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
        assert(payload);
        assert(cwist_livemsg_reply_write(reply, payload, CWIST_LIVEMSG_MAX_MESSAGE_BYTES) == 0);
        assert(cwist_livemsg_reply_write(reply, "x", 1) == -EMSGSIZE);
        free(payload);
        return CWIST_LIVEMSG_NO_REPLY;
    }
    return cwist_livemsg_reply_write(reply, msg, strlen(msg));
}

static void raw_on_connect(cwist_livemsg_peer *peer, void *user_data) {
    (void)user_data;
    assert(cwist_livemsg_send_raw(peer, "welcome", 7) == 0);
}

static cwist_livemsg_config raw_config(void) {
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WS);
    config.port = (uint16_t)raw_port;
    config.ws_path = "/raw";
    config.process = raw_process;
    config.respond = raw_respond;
    config.on_connect = raw_on_connect;
    return config;
}

static int json_process(cwist_livemsg_peer *peer, const void *msg, size_t len, void **result,
                        void *user_data) {
    (void)peer;
    (void)len;
    (void)user_data;
    *result = (void *)msg;
    return CWIST_LIVEMSG_OK;
}

static int json_respond(cwist_livemsg_peer *peer, void *result, cwist_livemsg_reply *reply,
                        void *user_data) {
    (void)peer;
    (void)user_data;
    return cwist_livemsg_reply_set_json(reply, result);
}

static void json_on_connect(cwist_livemsg_peer *peer, void *user_data) {
    (void)user_data;
    cJSON *welcome = cJSON_CreateObject();
    assert(welcome);
    assert(cJSON_AddStringToObject(welcome, "type", "welcome"));
    assert(cwist_livemsg_send_json(peer, welcome) == 0);
    cJSON_Delete(welcome);
}

static cwist_livemsg_config json_config(int argc, char **argv) {
    (void)argc;
    (void)argv;
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WS);
    config.port = (uint16_t)json_port;
    config.ws_path = "/json";
    config.codec = CWIST_LIVEMSG_CODEC_JSON;
    config.process = json_process;
    config.respond = json_respond;
    config.on_connect = json_on_connect;
    return config;
}

static cwist_livemsg_config invalid_config(void) {
    cwist_livemsg_config config = raw_config();
    config.transport = CWIST_LIVEMSG_TRANSPORT_WS | CWIST_LIVEMSG_TRANSPORT_WEBRTC;
    return config;
}

static pid_t start_raw_server(void) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int rc = cwist_livemsg_serve(raw_config);
        _exit(rc == 0 ? 0 : 100);
    }
    return pid;
}

static pid_t start_json_server(void) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char arg0[] = "test_livemsg";
        char *argv[] = {arg0, NULL};
        int rc = cwist_livemsg_serve_args(1, argv, json_config);
        _exit(rc == 0 ? 0 : 100);
    }
    return pid;
}

static void stop_server(pid_t pid) {
    assert(kill(pid, SIGTERM) == 0);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_invalid_and_lifecycle(void) {
    cwist_livemsg_config_init(NULL, 0);
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WEBRTC);
    assert(!strcmp(config.ws_path, "/"));
    assert(!strcmp(config.signaling_path, "/signal"));
    assert(cwist_livemsg_run(NULL) == -EINVAL);
    cwist_livemsg_destroy(NULL);
    assert(cwist_livemsg_peer_ctx(NULL) == NULL);
    assert(cwist_livemsg_alloc(NULL, 1) == NULL);
    assert(cwist_livemsg_reply_write(NULL, "x", 1) == -EINVAL);
    assert(cwist_livemsg_reply_set_json(NULL, NULL) == -EINVAL);
    assert(cwist_livemsg_send_raw(NULL, "x", 1) == -EINVAL);
    assert(cwist_livemsg_send_json(NULL, NULL) == -EINVAL);
    cwist_livemsg_foreach_peer(NULL, NULL, NULL);
    assert(cwist_livemsg_webrtc_offer(NULL, NULL, NULL) == -EINVAL);
    char *answer = (char *)1;
    assert(cwist_livemsg_webrtc_offer(NULL, "bad", &answer) == -EINVAL && !answer);
    assert(cwist_livemsg_serve(NULL) == -EINVAL);
    assert(cwist_livemsg_serve_args(0, NULL, NULL) == -EINVAL);
    assert(cwist_livemsg_serve(invalid_config) == -ENOTSUP);

    config = raw_config();
    config.process = NULL;
    errno = 0;
    assert(!cwist_livemsg_create(&config) && errno == EINVAL);
    config = raw_config();
    cwist_livemsg_ctx *ctx = cwist_livemsg_create(&config);
    assert(ctx);
    cwist_livemsg_destroy(ctx);
    puts("configuration and lifecycle: ok");
}

static void test_raw_server(void) {
    pid_t server = start_raw_server();
    int a = ws_connect((uint16_t)raw_port, "/raw");
    int b = ws_connect((uint16_t)raw_port, "/raw");
    expect_frame(a, "welcome", 7);
    expect_frame(b, "welcome", 7);

    ws_write(a, CWIST_WS_FRAME_BINARY, "echo", 4);
    expect_frame(a, "echo", 4);
    ws_write(a, CWIST_WS_FRAME_BINARY, "noreply", 7);
    ws_write(a, CWIST_WS_FRAME_BINARY, "after", 5);
    expect_frame(a, "after", 5);
    ws_write(a, CWIST_WS_FRAME_BINARY, "too-big", 7);
    ws_write(a, CWIST_WS_FRAME_BINARY, "after", 5);
    expect_frame(a, "after", 5);
    ws_write(a, CWIST_WS_FRAME_BINARY, "max", 3);
    client_frame max = ws_read(a);
    assert(max.len == CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
    frame_free(&max);

    ws_write(a, CWIST_WS_FRAME_BINARY, "broadcast", 9);
    expect_frame(a, "broadcast", 9);
    expect_frame(b, "broadcast", 9);
    puts("raw echo, no-reply, limits, arena, and broadcast: ok");

    enum { CLIENTS = 6 };
    int clients[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) {
        clients[i] = ws_connect((uint16_t)raw_port, "/raw");
        expect_frame(clients[i], "welcome", 7);
        char msg[16];
        int len = snprintf(msg, sizeof(msg), "client-%d", i);
        ws_write(clients[i], CWIST_WS_FRAME_BINARY, msg, (size_t)len);
    }
    for (int i = 0; i < CLIENTS; i++) {
        char msg[16];
        int len = snprintf(msg, sizeof(msg), "client-%d", i);
        expect_frame(clients[i], msg, (size_t)len);
        close(clients[i]);
    }
    puts("concurrent sessions: ok");

    int receive_buffer = 4096;
    assert(setsockopt(b, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);
    ws_write(b, CWIST_WS_FRAME_BINARY, "fill", 4);
    sleep_ms(200);
    size_t flooded = 0;
    for (;;) {
        struct pollfd pfd = {.fd = b, .events = POLLIN};
        int ready = poll(&pfd, 1, 250);
        assert(ready >= 0);
        if (!ready) break;
        client_frame frame = ws_read(b);
        assert(frame.len == CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
        flooded += frame.len;
        frame_free(&frame);
    }
    assert(flooded >= 3u * 1024u * 1024u);
    ws_write(b, CWIST_WS_FRAME_BINARY, "check-full", 10);
    expect_frame(b, "full", 4);
    printf("non-blocking send queue: ok (%zu bytes drained)\n", flooded);
    close(a);
    close(b);
    stop_server(server);
}

static void test_json_server(void) {
    pid_t server = start_json_server();
    int fd = ws_connect((uint16_t)json_port, "/json");
    client_frame welcome = ws_read(fd);
    cJSON *root = cJSON_ParseWithLength((char *)welcome.payload, welcome.len);
    assert(root && cJSON_IsString(cJSON_GetObjectItem(root, "type")));
    cJSON_Delete(root);
    frame_free(&welcome);

    const char request[] = "{\"value\":42}";
    ws_write(fd, CWIST_WS_FRAME_TEXT, request, sizeof(request) - 1);
    client_frame response = ws_read(fd);
    root = cJSON_ParseWithLength((char *)response.payload, response.len);
    cJSON *value = root ? cJSON_GetObjectItem(root, "value") : NULL;
    assert(cJSON_IsNumber(value) && value->valueint == 42);
    cJSON_Delete(root);
    frame_free(&response);
    close(fd);

    fd = ws_connect((uint16_t)json_port, "/json");
    client_frame ignored = ws_read(fd);
    frame_free(&ignored);
    ws_write(fd, CWIST_WS_FRAME_TEXT, "{bad", 4);
    client_frame close_frame = ws_read(fd);
    assert(close_frame.opcode == CWIST_WS_FRAME_CLOSE && close_frame.len == 2);
    assert(close_frame.payload[0] == 0x03 && close_frame.payload[1] == 0xef);
    frame_free(&close_frame);
    close(fd);

    fd = ws_connect((uint16_t)json_port, "/json");
    ignored = ws_read(fd);
    frame_free(&ignored);
    uint8_t *large = calloc(1, CWIST_LIVEMSG_MAX_MESSAGE_BYTES + 1);
    assert(large);
    ws_write(fd, CWIST_WS_FRAME_TEXT, large, CWIST_LIVEMSG_MAX_MESSAGE_BYTES + 1);
    free(large);
    close_frame = ws_read(fd);
    assert(close_frame.opcode == CWIST_WS_FRAME_CLOSE && close_frame.len == 2);
    assert(close_frame.payload[0] == 0x03 && close_frame.payload[1] == 0xf1);
    frame_free(&close_frame);
    close(fd);

    fd = ws_connect((uint16_t)json_port, "/json");
    ignored = ws_read(fd);
    frame_free(&ignored);
    static const char trailing[] = "{\"value\":42} trailing";
    ws_write(fd, CWIST_WS_FRAME_TEXT, trailing, sizeof(trailing) - 1);
    close_frame = ws_read(fd);
    assert(close_frame.opcode == CWIST_WS_FRAME_CLOSE && close_frame.len == 2);
    assert(close_frame.payload[0] == 0x03 && close_frame.payload[1] == 0xef);
    frame_free(&close_frame);
    close(fd);
    stop_server(server);
    puts("JSON echo and protocol error closes: ok");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    raw_port = (int)unused_tcp_port();
    json_port = (int)unused_tcp_port();
    assert(raw_port != json_port);
    test_invalid_and_lifecycle();
    test_raw_server();
    test_json_server();
    return 0;
}
