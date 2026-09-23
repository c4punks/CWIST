/* Experimental graphql-ws subscriptions (v3.7 Phase 4): drives the
 * graphql-transport-ws protocol over the non-blocking WebSocket transport
 * through socketpairs on a reactor thread, acting as a masked RFC 6455 client
 * from the main thread.  Covers the connection lifecycle (init/ack, ping/pong,
 * close codes 4400/4401/4409/4429), the topic broker (publish -> next fan-out,
 * topic filtering, unsubscribe), operation-level errors, and teardown leaks
 * (ASan/LSAN gate).
 */
#include <cwist/net/graphql/graphql_ws.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <cwist/net/websocket/websocket.h>
#include <cwist/sys/io/reactor.h>

#define TIMEOUT_MS 5000
#define CLOSE_BAD_REQUEST 4400
#define CLOSE_UNAUTHORIZED 4401
#define CLOSE_DUPLICATE_ID 4409
#define CLOSE_SUBSCRIBE_BEFORE_INIT 4429

static cwist_graphql_schema_t *g_schema;
static cwist_graphql_ws_t *g_gws;

/* Subscription resolver: subscribes to the `topic` argument (default
 * "default"); topic "reject" exercises the resolver-rejection path. */
static cwist_graphql_event_source_t *counter_subscribe(const cJSON *args, const cJSON *variables,
                                                       void *ctx) {
    (void)variables;
    (void)ctx;
    const char *topic = "default";
    if (args) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "topic");
        if (cJSON_IsString(t) && t->valuestring) topic = t->valuestring;
    }
    if (strcmp(topic, "reject") == 0) return NULL;
    cwist_graphql_event_source_t *src = cwist_graphql_event_source_create();
    if (!src) return NULL;
    if (!cwist_graphql_event_source_add_topic(src, topic)) {
        cwist_graphql_event_source_destroy(src);
        return NULL;
    }
    return src;
}

/* ---- masked RFC 6455 client helpers (main thread) ---- */

static void write_masked(int fd, uint8_t head0, const uint8_t *payload, size_t len) {
    uint8_t frame[1024];
    size_t pos = 0;
    frame[pos++] = head0;
    if (len < 126) {
        frame[pos++] = (uint8_t)(0x80 | len);
    } else {
        assert(len < 65536);
        frame[pos++] = (uint8_t)(0x80 | 126);
        frame[pos++] = (uint8_t)(len >> 8);
        frame[pos++] = (uint8_t)len;
    }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) frame[pos++] = payload[i] ^ mask[i % 4];
    /* MSG_NOSIGNAL: a frame can race a server-side close; SIGPIPE must not
     * kill the test process. */
    ssize_t off = 0;
    while ((size_t)off < pos) {
        ssize_t n = send(fd, frame + off, pos - (size_t)off, MSG_NOSIGNAL);
        assert(n > 0);
        off += n;
    }
}

static void write_text(int fd, const char *text) {
    write_masked(fd, 0x81, (const uint8_t *)text, strlen(text));
}

/* Read one server frame (never masked) with a poll timeout. */
static void read_server_frame(int fd, uint8_t *opcode, uint8_t *payload, size_t *out_len,
                              size_t cap) {
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
    assert(len < cap);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, payload + got, (size_t)len - got);
        assert(n > 0);
        got += (size_t)n;
    }
    payload[len] = '\0';
    *out_len = (size_t)len;
}

/* Read one JSON text message and return its parsed form (caller deletes). */
static cJSON *read_json_message(int fd) {
    uint8_t opcode;
    static uint8_t payload[8192];
    size_t len;
    read_server_frame(fd, &opcode, payload, &len, sizeof(payload));
    assert(opcode == CWIST_WS_FRAME_TEXT);
    cJSON *msg = cJSON_Parse((const char *)payload);
    assert(msg);
    return msg;
}

static void expect_close_code(int fd, uint16_t code) {
    uint8_t opcode;
    uint8_t payload[16];
    size_t len;
    read_server_frame(fd, &opcode, payload, &len, sizeof(payload));
    assert(opcode == CWIST_WS_FRAME_CLOSE);
    assert(len == 2);
    uint16_t got = (uint16_t)((payload[0] << 8) | payload[1]);
    assert(got == code);
}

/* The server must send nothing within `ms` milliseconds. */
static void expect_silence(int fd, int ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    assert(poll(&pfd, 1, ms) == 0);
}

static void expect_eof(int fd) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    assert(poll(&pfd, 1, TIMEOUT_MS) == 1);
    uint8_t b;
    assert(read(fd, &b, 1) == 0);
}

static void *reactor_thread(void *arg) {
    cwist_reactor_run((cwist_reactor_t *)arg);
    return NULL;
}

static void stop_cb(void *ctx) {
    cwist_reactor_stop((cwist_reactor_t *)ctx);
}

static int attach_conn(cwist_reactor_t *reactor) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(cwist_graphql_ws_attach(g_gws, fds[0], reactor, NULL, 0));
    return fds[1];
}

/* Publish `value` to `topic` and expect one `next` on `fd`. */
static void publish_and_expect_next(int fd, const char *topic, const char *op_id, int value) {
    cJSON *payload = cJSON_CreateObject();
    assert(payload);
    cJSON_AddNumberToObject(payload, "value", value);
    assert(cwist_graphql_publish(topic, payload) == 1);
    cJSON_Delete(payload);

    cJSON *msg = read_json_message(fd);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    assert(cJSON_IsString(type) && strcmp(type->valuestring, "next") == 0);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    assert(cJSON_IsString(id) && strcmp(id->valuestring, op_id) == 0);
    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(msg, "payload"), "data");
    assert(cJSON_IsObject(data));
    cJSON *counter = cJSON_GetObjectItemCaseSensitive(data, "counter");
    assert(cJSON_IsObject(counter));
    cJSON *v = cJSON_GetObjectItemCaseSensitive(counter, "value");
    assert(cJSON_IsNumber(v) && v->valuedouble == (double)value);
    cJSON_Delete(msg);
}

int main(void) {
    alarm(60);
    signal(SIGPIPE, SIG_IGN);

    g_schema = cwist_graphql_schema_create();
    assert(g_schema);
    g_gws = cwist_graphql_ws_create(g_schema);
    assert(g_gws);
    assert(cwist_graphql_ws_add_subscription(g_gws, "counter", counter_subscribe, NULL));

    cwist_reactor_t *reactor = cwist_reactor_create();
    assert(reactor);

    /* All connections attach before the reactor thread starts, matching the
     * harness pattern in tests/test_websocket_async.c. */
    int fd_early = attach_conn(reactor);    /* subscribe-before-init */
    int fd_ping = attach_conn(reactor);     /* ping-before-init */
    int fd_garbage = attach_conn(reactor);  /* malformed JSON */
    int fd_dup = attach_conn(reactor);      /* duplicate operation id */
    int fd = attach_conn(reactor);          /* main flow */

    pthread_t rt;
    assert(pthread_create(&rt, NULL, reactor_thread, reactor) == 0);

    /* 1. subscribe before connection_init closes with 4429. */
    write_text(fd_early,
               "{\"id\":\"1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription "
               "{ counter }\"}}");
    expect_close_code(fd_early, CLOSE_SUBSCRIBE_BEFORE_INIT);
    expect_eof(fd_early);
    close(fd_early);
    printf("1. subscribe before init -> close 4429: ok\n");

    /* 2. Any other pre-init message closes with 4401. */
    write_text(fd_ping, "{\"type\":\"ping\"}");
    expect_close_code(fd_ping, CLOSE_UNAUTHORIZED);
    expect_eof(fd_ping);
    close(fd_ping);
    printf("2. ping before init -> close 4401: ok\n");

    /* 3. A non-JSON text frame closes with 4400. */
    write_text(fd_garbage, "this is not json");
    expect_close_code(fd_garbage, CLOSE_BAD_REQUEST);
    expect_eof(fd_garbage);
    close(fd_garbage);
    printf("3. malformed JSON -> close 4400: ok\n");

    /* 4. Duplicate subscriber id closes with 4409. */
    write_text(fd_dup, "{\"type\":\"connection_init\"}");
    {
        cJSON *ack = read_json_message(fd_dup);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(ack, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "connection_ack") == 0);
        cJSON_Delete(ack);
    }
    write_text(fd_dup,
               "{\"id\":\"1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
               "counter(topic: \\\"t1\\\") }\"}}");
    expect_silence(fd_dup, 200); /* no per-operation response to subscribe */
    write_text(fd_dup,
               "{\"id\":\"1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
               "counter(topic: \\\"t2\\\") }\"}}");
    expect_close_code(fd_dup, CLOSE_DUPLICATE_ID);
    expect_eof(fd_dup);
    /* Teardown purges the still-active operation: a later publish on t1 must
     * find no subscribers once teardown has run. */
    usleep(200000);
    assert(cwist_graphql_publish("t1", NULL) == 0);
    close(fd_dup);
    printf("4. duplicate subscribe id -> close 4409: ok\n");

    /* 5. connection_init -> connection_ack. */
    write_text(fd, "{\"type\":\"connection_init\"}");
    {
        cJSON *ack = read_json_message(fd);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(ack, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "connection_ack") == 0);
        cJSON_Delete(ack);
    }
    printf("5. connection_init -> connection_ack: ok\n");

    /* 6. ping -> pong (payload echoed). */
    write_text(fd, "{\"type\":\"ping\",\"payload\":{\"tick\":7}}");
    {
        cJSON *pong = read_json_message(fd);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(pong, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "pong") == 0);
        cJSON *tick = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(pong, "payload"), "tick");
        assert(cJSON_IsNumber(tick) && tick->valuedouble == 7.0);
        cJSON_Delete(pong);
    }
    printf("6. ping -> pong: ok\n");

    /* 7. subscribe registers quietly; publish fans out one `next` per event. */
    write_text(fd, "{\"id\":\"1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
                   "counter(topic: \\\"t1\\\") }\"}}");
    expect_silence(fd, 200); /* no ack for subscribe */
    publish_and_expect_next(fd, "t1", "1", 1);
    printf("7. subscribe + publish -> next: ok\n");

    /* 8. Topic filtering: an unmatched topic reaches nobody. */
    assert(cwist_graphql_publish("nowhere", NULL) == 0);
    expect_silence(fd, 200);
    publish_and_expect_next(fd, "t1", "1", 2);
    printf("8. topic filtering: ok\n");

    /* 9. Operation-level failures send `error` and keep the connection. */
    write_text(fd, "{\"id\":\"u\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
                   "nope }\"}}");
    {
        cJSON *err = read_json_message(fd);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(err, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(err, "id");
        assert(cJSON_IsString(id) && strcmp(id->valuestring, "u") == 0);
        cJSON_Delete(err);
    }
    write_text(fd, "{\"id\":\"q\",\"type\":\"subscribe\",\"payload\":{\"query\":\"{ counter }\"}}");
    {
        cJSON *err = read_json_message(fd);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(err, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0);
        cJSON_Delete(err);
    }
    write_text(fd, "{\"id\":\"r\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
                   "counter(topic: \\\"reject\\\") }\"}}");
    {
        cJSON *err = read_json_message(fd);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(err, "type");
        assert(cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0);
        cJSON_Delete(err);
    }
    printf("9. unknown field / non-subscription / rejected resolver -> error: ok\n");

    /* 10. client `complete` unsubscribes: no further `next`, no response. */
    write_text(fd, "{\"id\":\"1\",\"type\":\"complete\"}");
    expect_silence(fd, 200);
    assert(cwist_graphql_publish("t1", NULL) == 0);
    expect_silence(fd, 200);
    printf("10. complete unsubscribes: ok\n");

    /* 11. A default-topic subscription receives its own events. */
    write_text(fd, "{\"id\":\"2\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription { "
                   "counter }\"}}");
    expect_silence(fd, 200);
    publish_and_expect_next(fd, "default", "2", 3);
    printf("11. default topic subscription: ok\n");

    /* 12. Socket close tears the subscription down; later publishes find
     *     nothing. */
    {
        uint8_t code[2] = {0x03, 0xE8}; /* 1000 */
        write_masked(fd, 0x88, code, 2);
        uint8_t opcode, payload[8];
        size_t len;
        read_server_frame(fd, &opcode, payload, &len, sizeof(payload));
        assert(opcode == CWIST_WS_FRAME_CLOSE);
        expect_eof(fd);
        usleep(200000); /* let the reactor thread run teardown */
        assert(cwist_graphql_publish("default", NULL) == 0);
        close(fd);
    }
    printf("12. socket close purges the subscription: ok\n");

    /* Clean shutdown: nothing may outlive the reactor (ASan leak gate). */
    cwist_reactor_post_t stop_node = {.cb = stop_cb, .ctx = reactor};
    assert(cwist_reactor_post(reactor, &stop_node));
    assert(pthread_join(rt, NULL) == 0);
    cwist_reactor_destroy(reactor);

    cwist_graphql_ws_destroy(g_gws);
    cwist_graphql_schema_destroy(g_schema);

    printf("graphql subscriptions (graphql-ws) test passed\n");
    return 0;
}
