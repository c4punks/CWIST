/** @file test_webrtc.c
 * @brief End-to-end WebRTC DataChannel tests over UDP loopback.
 *
 * Drives full ICE + DTLS + SCTP exchanges between in-process cwist webrtc
 * contexts (an answering ICE-lite endpoint and an offering peer built on the
 * internal client role) and checks:
 *   - STUN and SDP building blocks,
 *   - ping/pong in both directions, channel-open notification,
 *   - a message near the 256 KiB limit (SCTP partial delivery reassembly)
 *     and rejection of one over it,
 *   - the 4 MiB send-queue cap while SCTP is not up,
 *   - close from a foreign thread reaching the peer's close handler,
 *   - a ctx attached to a caller-run reactor (cwist_webrtc_ctx_new_on) and
 *     freed from another thread.
 */
#include <cwist/net/webrtc.h>
#include <cwist/sys/io/reactor.h>

#include "../src/net/webrtc/webrtc_internal.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BIG_LEN 200000

typedef struct test_peer {
    atomic_int got_ping;
    atomic_int got_pong;
    atomic_int got_channel;
    atomic_int got_big;
    atomic_int got_text;       /* "hi" received as a string */
    atomic_int got_empty_text; /* empty string */
    atomic_int got_empty_bin;  /* empty binary */
    atomic_int closed;
    char label[64];
} test_peer;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Wait until *flag reaches want, up to timeout_ms. */
static int wait_flag(atomic_int *flag, int want, int timeout_ms) {
    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    while (atomic_load(flag) < want) {
        if (now_ms() > end) return -1;
        usleep(1000);
    }
    return 0;
}

static int big_payload_ok(const uint8_t *data, size_t len) {
    if (len != BIG_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != (uint8_t)(i * 31u)) return 0;
    }
    return 1;
}

/* Track string/empty messages; echo them back with the same type. */
static int note_typed(test_peer *p, cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                      size_t len, cwist_webrtc_data_type type, int echo) {
    if (len == 0) {
        atomic_fetch_add(type == CWIST_WEBRTC_DATA_STRING ? &p->got_empty_text : &p->got_empty_bin,
                         1);
    } else if (len == 2 && memcmp(data, "hi", 2) == 0 && type == CWIST_WEBRTC_DATA_STRING) {
        atomic_fetch_add(&p->got_text, 1);
    } else {
        return 0;
    }
    if (echo) assert(cwist_webrtc_conn_send(conn, channel, data, len, type) == 0);
    return 1;
}

static void server_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, cwist_webrtc_data_type type, void *user) {
    test_peer *p = user;
    if (note_typed(p, conn, channel, data, len, type, 1)) return;
    if (len == 4 && memcmp(data, "ping", 4) == 0) {
        atomic_fetch_add(&p->got_ping, 1);
        /* Reply on the same channel from inside the callback (owner path). */
        assert(cwist_webrtc_conn_send(conn, channel, (const uint8_t *)"pong", 4,
                                      CWIST_WEBRTC_DATA_BINARY) == 0);
    } else if (big_payload_ok(data, len)) {
        atomic_fetch_add(&p->got_big, 1);
        /* Echo the big message back: exercises the owner-side queue. */
        assert(cwist_webrtc_conn_send(conn, channel, data, len, CWIST_WEBRTC_DATA_BINARY) == 0);
    }
}

static void client_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                              size_t len, cwist_webrtc_data_type type, void *user) {
    test_peer *p = user;
    if (note_typed(p, conn, channel, data, len, type, 0)) return;
    if (len == 4 && memcmp(data, "pong", 4) == 0)
        atomic_fetch_add(&p->got_pong, 1);
    else if (big_payload_ok(data, len))
        atomic_fetch_add(&p->got_big, 1);
}

static void peer_on_channel(cwist_webrtc_conn *conn, uint16_t channel, const char *label,
                            void *user) {
    test_peer *p = user;
    (void)conn;
    (void)channel;
    snprintf(p->label, sizeof(p->label), "%s", label);
    atomic_store(&p->got_channel, 1);
}

static void peer_on_close(cwist_webrtc_conn *conn, void *user) {
    test_peer *p = user;
    (void)conn;
    atomic_fetch_add(&p->closed, 1);
}

/* Answer an offer on @p server and dial it from @p client.  The offer carries
 * the client ctx's real ICE credentials, as a browser's would, so the server
 * adopts the conn it parked for the offer. */
static cwist_webrtc_conn *dial(cwist_webrtc_ctx *server, cwist_webrtc_ctx *client) {
    char offer[1024];
    assert(cwist_sdp_write_offer(offer, sizeof(offer),
                                 "00:11:22:33:44:55:66:77:88:99:"
                                 "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55",
                                 client->ice_ufrag, client->ice_pwd, "0") == 0);
    char answer[2048];
    assert(cwist_webrtc_handle_offer(server, offer, answer, sizeof(answer)) == 0);
    assert(strstr(answer, "a=ice-lite") != NULL);
    assert(strstr(answer, "a=setup:passive") != NULL);
    assert(strstr(answer, "webrtc-datachannel") != NULL);
    assert(strstr(answer, "a=candidate:") != NULL);
    cwist_sdp_info ans;
    assert(cwist_sdp_parse(answer, strlen(answer), &ans) == 0);

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(cwist_webrtc_ctx_port(server));
    remote.sin_addr.s_addr = htonl(0x7F000001);
    cwist_webrtc_conn *conn = cwist_webrtc_connect(client, &remote, ans.ice_ufrag, ans.ice_pwd);
    assert(conn != NULL);
    return conn;
}

typedef struct {
    cwist_reactor_post_t post;
    cwist_webrtc_ctx *ctx;
    atomic_int done;
    int parked;
    uint32_t nconns;
} table_probe;

static void table_probe_cb(void *arg) {
    table_probe *p = arg;
    p->parked = 0;
    for (cwist_webrtc_conn *c = p->ctx->parked; c; c = c->hnext) p->parked++;
    p->nconns = p->ctx->nconns;
    atomic_store(&p->done, 1);
}

/* Read the server's connection table on its reactor thread. */
static void probe_table(cwist_webrtc_ctx *ctx, int *parked, uint32_t *nconns) {
    table_probe p = {.ctx = ctx};
    p.post.cb = table_probe_cb;
    p.post.ctx = &p;
    assert(cwist_reactor_post(ctx->reactor, &p.post));
    assert(wait_flag(&p.done, 1, 5000) == 0);
    *parked = p.parked;
    *nconns = p.nconns;
}

static void unit_checks(void) {
    /* ICE/STUN: build, sign, validate, respond. */
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
        /* USERNAME is "<recipient>:<sender>"; the sender's ufrag comes back. */
        assert(cwist_ice_stun_get_remote_ufrag(req, (size_t)len, rfrag, sizeof(rfrag)) == 0);
        assert(strcmp(rfrag, "lfrag") == 0);

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

    /* SDP. */
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
}

/* Ping/pong, channel open, big message, close handler: both ctxs on their
 * own reactor threads. */
static void test_end_to_end(void) {
    cwist_webrtc_ctx *server = cwist_webrtc_ctx_new(0);
    assert(server != NULL);
    assert(cwist_webrtc_ctx_port(server) > 0);
    assert(strchr(cwist_webrtc_ctx_fingerprint(server), ':') != NULL);
    test_peer *sp = calloc(1, sizeof(*sp));
    test_peer *cp = calloc(1, sizeof(*cp));
    cwist_webrtc_ctx_set_message_handler(server, &server_on_message, sp);
    cwist_webrtc_ctx_set_channel_handler(server, &peer_on_channel, sp);
    cwist_webrtc_ctx_set_close_handler(server, &peer_on_close, sp);

    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(client != NULL);
    cwist_webrtc_ctx_set_message_handler(client, &client_on_message, cp);
    cwist_webrtc_ctx_set_close_handler(client, &peer_on_close, cp);

    cwist_webrtc_conn *conn = dial(server, client);

    /* Queued before SCTP is up; flushed once the association comes up. */
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"ping", 4, CWIST_WEBRTC_DATA_BINARY) ==
           0);
    assert(wait_flag(&sp->got_ping, 1, 10000) == 0);
    assert(wait_flag(&sp->got_channel, 1, 1000) == 0);
    assert(strcmp(sp->label, "dc1") == 0);
    assert(wait_flag(&cp->got_pong, 1, 5000) == 0);
    assert(cwist_webrtc_ctx_connection_count(server) == 1);
    assert(cwist_webrtc_ctx_connection_count(client) == 1);
    /* The conn parked by handle_offer was adopted, not duplicated. */
    int parked;
    uint32_t nconns;
    probe_table(server, &parked, &nconns);
    assert(parked == 0);
    assert(nconns == 1);

    /* Second exchange on the established channel. */
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"ping", 4, CWIST_WEBRTC_DATA_BINARY) ==
           0);
    assert(wait_flag(&sp->got_ping, 2, 5000) == 0);
    assert(wait_flag(&cp->got_pong, 2, 5000) == 0);

    /* Large message both ways: more than one SCTP read, reassembled. */
    uint8_t *big = malloc(BIG_LEN);
    for (size_t i = 0; i < BIG_LEN; i++) big[i] = (uint8_t)(i * 31u);
    assert(cwist_webrtc_conn_send(conn, 1, big, BIG_LEN, CWIST_WEBRTC_DATA_BINARY) == 0);
    assert(wait_flag(&sp->got_big, 1, 10000) == 0);
    assert(wait_flag(&cp->got_big, 1, 10000) == 0);
    free(big);

    /* Message types survive the round trip, including empty messages
     * (RFC 8831 empty PPIDs). */
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"hi", 2, CWIST_WEBRTC_DATA_STRING) ==
           0);
    assert(cwist_webrtc_conn_send(conn, 1, NULL, 0, CWIST_WEBRTC_DATA_STRING) == 0);
    assert(cwist_webrtc_conn_send(conn, 1, NULL, 0, CWIST_WEBRTC_DATA_BINARY) == 0);
    assert(wait_flag(&sp->got_text, 1, 5000) == 0);
    assert(wait_flag(&sp->got_empty_text, 1, 5000) == 0);
    assert(wait_flag(&sp->got_empty_bin, 1, 5000) == 0);
    assert(wait_flag(&cp->got_text, 1, 5000) == 0);
    assert(wait_flag(&cp->got_empty_text, 1, 5000) == 0);
    assert(wait_flag(&cp->got_empty_bin, 1, 5000) == 0);

    /* Over the advertised max-message-size: rejected up front. */
    uint8_t *huge = calloc(1, CWIST_WEBRTC_MAX_MESSAGE + 1);
    assert(cwist_webrtc_conn_send(conn, 1, huge, CWIST_WEBRTC_MAX_MESSAGE + 1,
                                  CWIST_WEBRTC_DATA_BINARY) == -1);
    free(huge);

    /* Close from this (foreign) thread: our close handler runs, the SCTP
     * ABORT reaches the server and its close handler runs too. */
    cwist_webrtc_conn_close(conn);
    assert(wait_flag(&cp->closed, 1, 5000) == 0);
    assert(wait_flag(&sp->closed, 1, 5000) == 0);
    assert(cwist_webrtc_ctx_connection_count(client) == 0);
    assert(cwist_webrtc_ctx_connection_count(server) == 0);
    /* The retained conn outlives its teardown; sends now fail fast. */
    assert(cwist_webrtc_conn_send(conn, 1, (const uint8_t *)"late", 4, CWIST_WEBRTC_DATA_BINARY) ==
           -1);
    cwist_webrtc_conn_release(conn);

    cwist_webrtc_ctx_free(client);
    cwist_webrtc_ctx_free(server);
    free(sp);
    free(cp);
}

/* Sends queue while SCTP is down, up to the 4 MiB cap. */
static void test_send_queue_cap(void) {
    cwist_webrtc_ctx *server = cwist_webrtc_ctx_new(0);
    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(server && client);
    /* No offer answered on server: it ignores our Binding requests, so the
     * conn stays in ICE and every send is queued. */
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(cwist_webrtc_ctx_port(server));
    remote.sin_addr.s_addr = htonl(0x7F000001);
    cwist_webrtc_conn *conn = cwist_webrtc_connect(client, &remote, "nouser", "nopassword");
    assert(conn);

    static uint8_t chunk[65536];
    size_t accepted = 0;
    int rc;
    while ((rc = cwist_webrtc_conn_send(conn, 1, chunk, sizeof(chunk), CWIST_WEBRTC_DATA_BINARY)) ==
           0)
        accepted += sizeof(chunk);
    assert(accepted == CWIST_WEBRTC_MAX_BUFFERED);
    assert(cwist_webrtc_conn_buffered_amount(conn) == CWIST_WEBRTC_MAX_BUFFERED);

    cwist_webrtc_ctx_free(client);
    /* Teardown dropped the queue. */
    assert(cwist_webrtc_conn_buffered_amount(conn) == 0);
    cwist_webrtc_conn_release(conn);
    cwist_webrtc_ctx_free(server);
}

static void *reactor_main(void *arg) {
    cwist_reactor_run(arg);
    return NULL;
}

/* A server ctx on a reactor this test runs itself, freed from this thread
 * while the reactor keeps running. */
static void test_external_reactor(void) {
    cwist_reactor_t *reactor = cwist_reactor_create();
    assert(reactor);
    cwist_webrtc_ctx *server = cwist_webrtc_ctx_new_on(reactor, 0);
    assert(server);
    test_peer *sp = calloc(1, sizeof(*sp));
    test_peer *cp = calloc(1, sizeof(*cp));
    cwist_webrtc_ctx_set_message_handler(server, &server_on_message, sp);
    cwist_webrtc_ctx_set_close_handler(server, &peer_on_close, sp);
    pthread_t th;
    assert(pthread_create(&th, NULL, reactor_main, reactor) == 0);

    cwist_webrtc_ctx *client = cwist_webrtc_ctx_new(0);
    assert(client);
    cwist_webrtc_ctx_set_message_handler(client, &client_on_message, cp);
    cwist_webrtc_conn *conn = dial(server, client);
    assert(cwist_webrtc_conn_send(conn, 2, (const uint8_t *)"ping", 4, CWIST_WEBRTC_DATA_BINARY) ==
           0);
    assert(wait_flag(&sp->got_ping, 1, 10000) == 0);
    assert(wait_flag(&cp->got_pong, 1, 5000) == 0);

    /* Freed off the reactor thread: waits for the teardown, which runs the
     * server's close handler on the reactor thread. */
    cwist_webrtc_ctx_free(server);
    assert(atomic_load(&sp->closed) == 1);

    cwist_reactor_stop(reactor);
    assert(pthread_join(th, NULL) == 0);
    cwist_reactor_destroy(reactor);

    cwist_webrtc_conn_release(conn);
    cwist_webrtc_ctx_free(client);
    free(sp);
    free(cp);
}

int main(void) {
    unit_checks();
    test_end_to_end();
    test_send_queue_cap();
    test_external_reactor();
    printf("test_webrtc: all assertions passed\n");
    return 0;
}
