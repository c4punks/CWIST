#include <cwist/livemsg.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/webrtc.h>

#include "../src/net/webrtc/webrtc_internal.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BIG_LEN 200000

static atomic_int connected;
static atomic_int disconnected;
static atomic_int got_welcome;
static atomic_int got_echo;
static atomic_int got_big;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void wait_flag(atomic_int *flag) {
    uint64_t deadline = now_ms() + 10000;
    while (!atomic_load(flag)) {
        assert(now_ms() < deadline);
        usleep(1000);
    }
}

static int app_process(cwist_livemsg_peer *peer, const void *msg, size_t len, void **result,
                       void *user_data) {
    (void)user_data;
    uint8_t *copy = cwist_livemsg_alloc(peer, len + sizeof(size_t));
    assert(copy);
    memcpy(copy, &len, sizeof(len));
    if (len) memcpy(copy + sizeof(len), msg, len);
    *result = copy;
    return CWIST_LIVEMSG_OK;
}

static int app_respond(cwist_livemsg_peer *peer, void *result, cwist_livemsg_reply *reply,
                       void *user_data) {
    (void)peer;
    (void)user_data;
    size_t len;
    memcpy(&len, result, sizeof(len));
    return cwist_livemsg_reply_write(reply, (char *)result + sizeof(len), len);
}

static void on_connect(cwist_livemsg_peer *peer, void *user_data) {
    (void)user_data;
    atomic_fetch_add(&connected, 1);
    assert(cwist_livemsg_send_raw(peer, "welcome", 7) == 0);
}

static void on_disconnect(cwist_livemsg_peer *peer, void *user_data) {
    (void)peer;
    (void)user_data;
    atomic_fetch_add(&disconnected, 1);
}

static bool big_ok(const uint8_t *data, size_t len) {
    if (len != BIG_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != (uint8_t)(i * 31u)) return false;
    }
    return true;
}

static void client_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type, void *user) {
    (void)conn;
    (void)channel;
    (void)type;
    (void)user;
    if (len == 7 && !memcmp(data, "welcome", 7))
        atomic_store(&got_welcome, 1);
    else if (len == 4 && !memcmp(data, "ping", 4))
        atomic_store(&got_echo, 1);
    else if (big_ok(data, len))
        atomic_store(&got_big, 1);
}

int main(void) {
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WEBRTC);
    config.signaling_path = NULL;
    config.process = app_process;
    config.respond = app_respond;
    config.on_connect = on_connect;
    config.on_disconnect = on_disconnect;
    cwist_livemsg_ctx *server = cwist_livemsg_create(&config);
    assert(server);

    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(client);
    cwist_webrtc_ctx_set_message_handler(client, client_message, NULL);

    char offer[1024];
    assert(cwist_sdp_write_offer(offer, sizeof(offer), cwist_webrtc_ctx_fingerprint(client),
                                 client->ice_ufrag, client->ice_pwd, "0") == 0);
    char *answer = NULL;
    assert(cwist_livemsg_webrtc_offer(server, offer, &answer) == 0);
    assert(answer && strstr(answer, "a=ice-lite"));
    cwist_sdp_info parsed;
    assert(cwist_sdp_parse(answer, strlen(answer), &parsed) == 0);
    unsigned port = 0;
    char *candidate = strstr(answer, "a=candidate:");
    assert(candidate && sscanf(candidate, "a=candidate:%*u %*u %*s %*u %*s %u", &port) == 1);
    assert(port > 0);
    cwist_free(answer);
    assert(cwist_livemsg_webrtc_offer(server, "bad", &answer) < 0 && !answer);

    struct sockaddr_in remote = {.sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    cwist_webrtc_conn *conn =
        cwist_webrtc_connect(client, &remote, parsed.ice_ufrag, parsed.ice_pwd, parsed.fingerprint);
    assert(conn);
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"ping", 4, CWIST_WEBRTC_DATA_BINARY) ==
           0);
    wait_flag(&got_welcome);
    wait_flag(&got_echo);
    assert(atomic_load(&connected) == 1);

    uint8_t *big = malloc(BIG_LEN);
    assert(big);
    for (size_t i = 0; i < BIG_LEN; i++) big[i] = (uint8_t)(i * 31u);
    assert(cwist_webrtc_conn_send(conn, 1, big, BIG_LEN, CWIST_WEBRTC_DATA_BINARY) == 0);
    free(big);
    wait_flag(&got_big);

    cwist_webrtc_conn_close(conn);
    wait_flag(&disconnected);
    cwist_webrtc_conn_release(conn);
    cwist_webrtc_ctx_free(client);
    cwist_livemsg_destroy(server);
    puts("WebRTC external signaling, push, echo, large message, and teardown: ok");
    return 0;
}
