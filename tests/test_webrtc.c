/** @file test_webrtc.c
 * @brief End-to-end WebRTC DataChannel test over UDP loopback.
 *
 * Drives a full ICE + DTLS + SCTP exchange between two in-process cwist
 * webrtc contexts (one answering ICE-lite endpoint, one offering peer built
 * on the internal client role) and exchanges DataChannel messages in both
 * directions.
 */
#include <cwist/net/webrtc.h>

#include "../src/net/webrtc/webrtc_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct test_peer {
    int got_ping;
    int got_pong;
    int got_channel;
    char label[64];
    cwist_webrtc_conn *conn;
} test_peer;

static void server_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, void *user) {
    test_peer *p = user;
    if (len == 4 && memcmp(data, "ping", 4) == 0) {
        p->got_ping = 1;
        /* Reply on the same channel: server -> client direction. */
        assert(cwist_webrtc_conn_send(conn, channel, (const uint8_t *)"pong", 4,
                                      CWIST_WEBRTC_DATA_BINARY) == 0);
    }
}

static void client_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, void *user) {
    test_peer *p = user;
    p->conn = conn;
    if (len == 4 && memcmp(data, "pong", 4) == 0)
        p->got_pong = 1;
    (void)channel;
}

static void peer_on_channel(cwist_webrtc_conn *conn, uint16_t channel, const char *label,
                            void *user) {
    test_peer *p = user;
    p->conn = conn;
    p->got_channel = 1;
    snprintf(p->label, sizeof(p->label), "%s", label);
    (void)channel;
}

int main(void) {
    /* ICE/STUN unit checks: build, sign, validate, respond. */
    {
        uint8_t req[512];
        uint8_t txid[12];
        memset(txid, 0xAB, sizeof(txid));
        int len = cwist_ice_stun_build_request(req, sizeof(req), txid, "rfrag:lfrag");
        assert(len > 0);
        len = cwist_ice_stun_sign_request(req, sizeof(req), (size_t)len, "testpassword0123456789");
        assert(len > 0);
        assert(cwist_ice_stun_is_message(req, (size_t)len));
        assert(cwist_ice_stun_is_binding_request(req, (size_t)len));
        assert(cwist_ice_stun_has_use_candidate(req, (size_t)len));
        assert(cwist_ice_stun_validate_request(req, (size_t)len, "testpassword0123456789") == 1);
        assert(cwist_ice_stun_validate_request(req, (size_t)len, "wrongpassword000000000") == 0);
        char rfrag[64];
        assert(cwist_ice_stun_get_remote_ufrag(req, (size_t)len, rfrag, sizeof(rfrag)) == 0);
        assert(strcmp(rfrag, "rfrag") == 0);

        struct sockaddr_in mapped;
        memset(&mapped, 0, sizeof(mapped));
        mapped.sin_family = AF_INET;
        mapped.sin_port = htons(54321);
        mapped.sin_addr.s_addr = htonl(0x7F000001);
        uint8_t resp[512];
        int rlen = cwist_ice_stun_build_response(resp, sizeof(resp), req, (size_t)len, &mapped,
                                                 "testpassword0123456789");
        assert(rlen > 0);
        struct sockaddr_in parsed;
        assert(cwist_ice_stun_parse_response(resp, (size_t)rlen, txid, "testpassword0123456789",
                                             &parsed) == 1);
        assert(parsed.sin_port == htons(54321));
        assert(parsed.sin_addr.s_addr == htonl(0x7F000001));
    }

    /* SDP unit checks. */
    {
        char offer[1024];
        assert(cwist_sdp_write_offer(offer, sizeof(offer),
                                     "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                                     "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99",
                                     "offrfrag", "offpassword0123456789012", "0") == 0);
        cwist_sdp_info info;
        assert(cwist_sdp_parse(offer, strlen(offer), &info) == 0);
        assert(strcmp(info.ice_ufrag, "offrfrag") == 0);
        assert(strcmp(info.ice_pwd, "offpassword0123456789012") == 0);
        assert(strcmp(info.setup, "active") == 0);
        assert(info.has_lite == 0);
    }

    /* End-to-end: offer/answer + ICE + DTLS + SCTP + DataChannel ping/pong. */
    cwist_webrtc_ctx *server = cwist_webrtc_ctx_new(0);
    assert(server != NULL);
    assert(cwist_webrtc_ctx_port(server) > 0);
    assert(strchr(cwist_webrtc_ctx_fingerprint(server), ':') != NULL);

    test_peer server_peer = { 0 };
    cwist_webrtc_ctx_set_message_handler(server, &server_on_message, &server_peer);
    cwist_webrtc_ctx_set_channel_handler(server, &peer_on_channel, &server_peer);

    char offer[1024];
    assert(cwist_sdp_write_offer(offer, sizeof(offer), "00:11:22:33:44:55:66:77:88:99:"
                                                       "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55",
                                 "browserfrag", "browserpassword0123456789012", "0") == 0);
    char answer[2048];
    assert(cwist_webrtc_handle_offer(server, offer, answer, sizeof(answer)) == 0);
    assert(strstr(answer, "a=ice-lite") != NULL);
    assert(strstr(answer, "a=setup:passive") != NULL);
    assert(strstr(answer, "webrtc-datachannel") != NULL);
    assert(strstr(answer, "a=candidate:") != NULL);

    cwist_sdp_info ans;
    assert(cwist_sdp_parse(answer, strlen(answer), &ans) == 0);

    test_peer client_peer = { 0 };
    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(client != NULL);
    cwist_webrtc_ctx_set_message_handler(client, &client_on_message, &client_peer);

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(cwist_webrtc_ctx_port(server));
    remote.sin_addr.s_addr = htonl(0x7F000001);

    cwist_webrtc_conn *conn = cwist_webrtc_connect(client, &remote, ans.ice_ufrag, ans.ice_pwd);
    assert(conn != NULL);

    /* Open channel 1 from the offering side and send "ping". */
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"ping", 4,
                                  CWIST_WEBRTC_DATA_BINARY) == 0);

    int waited_ms = 0;
    while (waited_ms < 10000 &&
           !(server_peer.got_channel && client_peer.got_pong && server_peer.got_ping)) {
        usleep(10000);
        waited_ms += 10;
    }

    assert(server_peer.got_ping);
    assert(server_peer.got_channel);
    assert(strcmp(server_peer.label, "dc1") == 0);
    assert(client_peer.got_pong);

    /* Second message on the now-established channel, both directions again. */
    server_peer.got_ping = 0;
    client_peer.got_pong = 0;
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"ping", 4,
                                  CWIST_WEBRTC_DATA_BINARY) == 0);
    waited_ms = 0;
    while (waited_ms < 5000 && !(client_peer.got_pong && server_peer.got_ping)) {
        usleep(10000);
        waited_ms += 10;
    }
    assert(server_peer.got_ping);
    assert(client_peer.got_pong);

    assert(cwist_webrtc_ctx_connection_count(server) >= 1);

    cwist_webrtc_ctx_free(client);
    cwist_webrtc_ctx_free(server);

    printf("test_webrtc: all assertions passed\n");
    return 0;
}
