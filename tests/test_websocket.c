#include <cwist/net/websocket/websocket.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/seq/seq.h>
#include <cwist/core/mem/alloc.h>
#include "../src/net/websocket/ws_utils.h"
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

void test_sha1() {
    printf("Testing SHA1...\n");
    const char *input = "abc";
    uint8_t hash[20];
    sha1((const uint8_t *)input, strlen(input), hash);

    printf("Computed Hash: ");
    print_hex(hash, 20);

    // Expected for "abc": a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
    uint8_t expected[] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                          0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};

    assert(memcmp(hash, expected, 20) == 0);
    printf("SHA1 Test Passed.\n");
}

void test_handshake_key_generation() {
    test_sha1();
    printf("Testing Handshake Key Generation...\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");

    cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
    assert(ws != NULL);
    assert(ws->fd == sv[0]);
    assert(req->upgraded == true);

    // Read response from sv[1]
    char buffer[1024];
    int len = read(sv[1], buffer, sizeof(buffer) - 1);
    buffer[len] = '\0';

    printf("Response:\n%s\n", buffer);

    assert(strstr(buffer, "101 Switching Protocols") != NULL);
    assert(strstr(buffer, "Upgrade: websocket") != NULL);
    assert(strstr(buffer, "Connection: Upgrade") != NULL);
    assert(strstr(buffer, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    printf("Handshake Test Passed.\n");
}

/* RFC 6455 section 4.2.1: the client MUST include Sec-WebSocket-Version: 13.
 * Proves the rejection side of the fix, not just that a well-formed
 * request still works. */
void test_handshake_rejects_bad_version() {
    printf("Testing handshake rejects missing/wrong Sec-WebSocket-Version...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    /* No Sec-WebSocket-Version header at all. */
    assert(cwist_websocket_upgrade(req, sv[0]) == NULL);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Version",
                          "8"); /* pre-RFC6455 draft version */
    assert(cwist_websocket_upgrade(req, sv[0]) == NULL);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    printf("Passed version rejection.\n");
}

static void assert_handshake(const char *connection, const char *key, bool expected) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", connection);
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", key);
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");
    cwist_http_response *res = cwist_http_response_create();
    bool accepted = cwist_websocket_upgrade_response(req, res);
    assert(accepted == expected);
    if (accepted) {
        assert(res->status_code == 101);
        assert(strcmp(cwist_http_header_get(res->headers, "Sec-WebSocket-Accept"),
                      "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    }
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void test_handshake_validation(void) {
    const char *valid_key = "dGhlIHNhbXBsZSBub25jZQ==";
    assert_handshake("not-an-upgrade", valid_key, false);
    assert_handshake("upgrade.invalid", valid_key, false);
    assert_handshake("keep-alive, upgrade.invalid", valid_key, false);
    assert_handshake("Upgrade", "x", false);
    assert_handshake("Upgrade", "AAAAAAAAAAAAAAAAAAAA", false);
    assert_handshake("Upgrade", "!!!!!!!!!!!!!!!!!!!!!!!!", false);
    assert_handshake("Upgrade", "dGhlIHNhbXBsZSBub25jZQ=A", false);
    assert_handshake("keep-alive, Upgrade", valid_key, true);
    assert_handshake("upgrade", valid_key, true);
    printf("WebSocket handshake validation tests passed.\n");
}

static void send_masked_frame(int fd, uint8_t head0, const uint8_t *payload, size_t len) {
    assert(len < 126);
    uint8_t frame[140];
    size_t pos = 0;
    frame[pos++] = head0;
    frame[pos++] = (uint8_t)(0x80 | len);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) {
        frame[pos++] = payload[i] ^ mask[i % 4];
    }
    assert(write(fd, frame, pos) == (ssize_t)pos);
}

static void send_masked_extended_frame(int fd, uint8_t head0, const uint8_t *payload, size_t len,
                                       uint8_t length_code) {
    uint8_t frame[65536 + 14];
    size_t pos = 0;
    assert(length_code == 126 || length_code == 127);
    assert(length_code != 126 || len <= 65535);
    frame[pos++] = head0;
    frame[pos++] = 0x80 | length_code;
    size_t ext_len = length_code == 126 ? 2 : 8;
    for (size_t i = 0; i < ext_len; i++) frame[pos++] = (uint8_t)(len >> ((ext_len - i - 1) * 8));
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) frame[pos++] = payload[i] ^ mask[i % 4];
    assert(write(fd, frame, pos) == (ssize_t)pos);
}

static void send_masked_binary_frame(int fd, const uint8_t *payload, size_t len) {
    send_masked_frame(fd, 0x82, payload, len); /* FIN=1, BINARY */
}

/* RFC 6455 section 5.4: a CONTINUATION frame outside an active fragmented
 * message is a protocol error and the connection must be failed, whether
 * FIN is set or not (issue #158). */
static void test_orphan_continuation_rejected(void) {
    printf("Testing orphan CONTINUATION rejection...\n");

    const uint8_t payload[] = {'x'};

    /* FIN=1 orphan CONTINUATION must be rejected. */
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        cwist_http_request *req = cwist_http_request_create();
        cwist_http_header_add(&req->headers, "Connection", "Upgrade");
        cwist_http_header_add(&req->headers, "Upgrade", "websocket");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");
        cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
        assert(ws != NULL);
        char buf[1024];
        assert(read(sv[1], buf, sizeof(buf)) > 0);

        send_masked_frame(sv[1], 0x80, payload, 1); /* FIN=1, CONTINUATION */
        assert(cwist_websocket_receive(ws) == NULL);

        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    /* FIN=0 orphan CONTINUATION must be rejected. */
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        cwist_http_request *req = cwist_http_request_create();
        cwist_http_header_add(&req->headers, "Connection", "Upgrade");
        cwist_http_header_add(&req->headers, "Upgrade", "websocket");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");
        cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
        assert(ws != NULL);
        char buf[1024];
        assert(read(sv[1], buf, sizeof(buf)) > 0);

        send_masked_frame(sv[1], 0x00, payload, 1); /* FIN=0, CONTINUATION */
        assert(cwist_websocket_receive(ws) == NULL);

        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    /* A well-formed fragmented message still reassembles correctly. */
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        cwist_http_request *req = cwist_http_request_create();
        cwist_http_header_add(&req->headers, "Connection", "Upgrade");
        cwist_http_header_add(&req->headers, "Upgrade", "websocket");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");
        cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
        assert(ws != NULL);
        char buf[1024];
        assert(read(sv[1], buf, sizeof(buf)) > 0);

        send_masked_frame(sv[1], 0x01, (const uint8_t *)"he", 2); /* FIN=0 TEXT */
        send_masked_frame(sv[1], 0x80, (const uint8_t *)"llo", 3); /* FIN=1 CONTINUATION */
        cwist_ws_frame *frame = cwist_websocket_receive(ws);
        assert(frame != NULL);
        assert(frame->opcode == CWIST_WS_FRAME_TEXT);
        assert(frame->payload_len == 5);
        assert(memcmp(frame->payload, "hello", 5) == 0);
        cwist_websocket_frame_destroy(frame);

        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    printf("Orphan CONTINUATION rejection test passed.\n");
}

static cwist_websocket *upgrade_for_frames(int sv[2], cwist_http_request **out_req);

static void test_reserved_opcodes_rejected(void) {
    const uint8_t opcodes[] = {0x83, 0x8B};
    for (size_t i = 0; i < 2; i++) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        cwist_http_request *req;
        cwist_websocket *ws = upgrade_for_frames(sv, &req);
        send_masked_frame(sv[1], opcodes[i], NULL, 0);
        assert(cwist_websocket_receive(ws) == NULL);
        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }
    printf("Reserved WebSocket opcode rejection test passed.\n");
}

static cwist_websocket *upgrade_for_frames(int sv[2], cwist_http_request **out_req) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");
    cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
    assert(ws != NULL);
    char buf[1024];
    assert(read(sv[1], buf, sizeof(buf)) > 0);
    *out_req = req;
    return ws;
}

static void test_fragmentation_state(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    cwist_http_request *req;
    cwist_websocket *ws = upgrade_for_frames(sv, &req);
    send_masked_frame(sv[1], 0x01, NULL, 0);
    send_masked_frame(sv[1], 0x00, NULL, 0);
    send_masked_frame(sv[1], 0x80, (const uint8_t *)"x", 1);
    cwist_ws_frame *frame = cwist_websocket_receive(ws);
    assert(frame != NULL && frame->opcode == CWIST_WS_FRAME_TEXT);
    assert(frame->payload_len == 1 && frame->payload[0] == 'x');
    cwist_websocket_frame_destroy(frame);
    send_masked_frame(sv[1], 0x01, NULL, 0);
    send_masked_frame(sv[1], 0x80, NULL, 0);
    frame = cwist_websocket_receive(ws);
    assert(frame != NULL && frame->opcode == CWIST_WS_FRAME_TEXT);
    assert(frame->payload_len == 0);
    cwist_websocket_frame_destroy(frame);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ws = upgrade_for_frames(sv, &req);
    send_masked_frame(sv[1], 0x01, (const uint8_t *)"a", 1);
    send_masked_frame(sv[1], 0x81, (const uint8_t *)"b", 1);
    assert(cwist_websocket_receive(ws) == NULL);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    const uint8_t fragmented_control[] = {0x08, 0x09, 0x0A};
    for (size_t i = 0; i < sizeof(fragmented_control); i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ws = upgrade_for_frames(sv, &req);
        send_masked_frame(sv[1], fragmented_control[i], (const uint8_t *)"x", 1);
        assert(cwist_websocket_receive(ws) == NULL);
        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ws = upgrade_for_frames(sv, &req);
    send_masked_frame(sv[1], 0x01, (const uint8_t *)"a", 1);
    send_masked_frame(sv[1], 0x89, (const uint8_t *)"p", 1);
    frame = cwist_websocket_receive(ws);
    assert(frame != NULL && frame->opcode == CWIST_WS_FRAME_PING);
    cwist_websocket_frame_destroy(frame);
    send_masked_frame(sv[1], 0x80, (const uint8_t *)"b", 1);
    frame = cwist_websocket_receive(ws);
    assert(frame != NULL && frame->opcode == CWIST_WS_FRAME_TEXT);
    assert(frame->payload_len == 2 && memcmp(frame->payload, "ab", 2) == 0);
    cwist_websocket_frame_destroy(frame);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);
    printf("WebSocket blocking fragmentation state tests passed.\n");
}

/* Read one unmasked server frame from @p fd. */
static void read_server_frame(int fd, uint8_t *head0, uint8_t *payload, size_t cap, size_t *len) {
    uint8_t head[2];
    assert(read(fd, head, 2) == 2);
    assert((head[1] & 0x80) == 0); /* server frames are not masked */
    size_t plen = head[1] & 0x7f;
    if (plen == 126) {
        uint8_t ext[2];
        assert(read(fd, ext, 2) == 2);
        plen = (size_t)ext[0] << 8 | ext[1];
    }
    assert(plen <= cap);
    size_t got = 0;
    while (got < plen) {
        ssize_t n = read(fd, payload + got, plen - got);
        assert(n > 0);
        got += (size_t)n;
    }
    *head0 = head[0];
    *len = plen;
}

static bool fd_has_pending_bytes(int fd) {
    int flags = fcntl(fd, F_GETFL);
    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    uint8_t byte;
    ssize_t n = read(fd, &byte, 1);
    bool pending = n > 0;
    assert(pending || errno == EAGAIN || errno == EWOULDBLOCK);
    assert(fcntl(fd, F_SETFL, flags) == 0);
    return pending;
}

static void test_websocket_send_fragments(void) {
    int sv[2];
    cwist_http_request *req;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    cwist_websocket *ws = upgrade_for_frames(sv, &req);

    uint8_t head0;
    uint8_t payload[512];
    size_t len;

    /* Multi-fragment TEXT: first keeps the opcode, the rest are CONTINUATION,
     * only the last has FIN. The middle fragment splits a UTF-8 sequence. */
    const char *text = "caf\xc3\xa9!";
    cwist_ws_fragment parts[] = {
        {(const uint8_t *)text, 4}, {(const uint8_t *)text + 4, 0}, {(const uint8_t *)text + 4, 2}};
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_TEXT, parts, 3) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x01 && len == 4 && memcmp(payload, "caf\xc3", 4) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x00 && len == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x80 && len == 2 && memcmp(payload, "\xa9!", 2) == 0);

    /* BINARY keeps its opcode; a 300-byte fragment uses the 16-bit length. */
    uint8_t big[300];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    cwist_ws_fragment bin[] = {{big, 300}, {big, 1}};
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_BINARY, bin, 2) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x02 && len == 300 && memcmp(payload, big, 300) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x80 && len == 1 && payload[0] == 0);

    /* One fragment is an ordinary FIN frame; zero fragments send an empty message. */
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_BINARY, bin + 1, 1) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x82 && len == 1);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_TEXT, NULL, 0) == 0);
    read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
    assert(head0 == 0x81 && len == 0);

    /* Invalid sequencing and arguments are rejected before anything is written. */
    assert(cwist_websocket_send_fragments(NULL, CWIST_WS_FRAME_TEXT, parts, 3) == -1);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_CONTINUATION, parts, 3) == -1);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_PING, parts, 1) == -1);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_CLOSE, NULL, 0) == -1);
    assert(cwist_websocket_send_fragments(ws, (cwist_ws_opcode_t)0x3, parts, 1) == -1);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_TEXT, NULL, 2) == -1);
    cwist_ws_fragment bad[] = {{(const uint8_t *)"ok", 2}, {NULL, 5}};
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_BINARY, bad, 2) == -1);
    assert(!fd_has_pending_bytes(sv[1]));

    /* A peer that reassembles the frames gets the original message back. */
    int peer[2];
    cwist_http_request *peer_req;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0);
    cwist_websocket *receiver = upgrade_for_frames(peer, &peer_req);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_TEXT, parts, 3) == 0);
    for (int i = 0; i < 3; i++) {
        read_server_frame(sv[1], &head0, payload, sizeof(payload), &len);
        send_masked_frame(peer[1], head0, payload, len);
        if (i == 1) send_masked_frame(peer[1], 0x89, (const uint8_t *)"p", 1);
    }
    cwist_ws_frame *frame = cwist_websocket_receive(receiver);
    assert(frame && frame->opcode == CWIST_WS_FRAME_PING);
    cwist_websocket_frame_destroy(frame);
    frame = cwist_websocket_receive(receiver);
    assert(frame && frame->opcode == CWIST_WS_FRAME_TEXT && frame->fin);
    assert(frame->payload_len == 6 && memcmp(frame->payload, text, 6) == 0);
    cwist_websocket_frame_destroy(frame);
    cwist_websocket_destroy(receiver);
    cwist_http_request_destroy(peer_req);
    close(peer[0]);
    close(peer[1]);

    /* Send failures are reported, and a closed wrapper refuses to send. */
    close(sv[1]);
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_BINARY, bin, 2) == -1);
    ws->is_closed = true;
    assert(cwist_websocket_send_fragments(ws, CWIST_WS_FRAME_BINARY, bin, 2) == -1);

    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    printf("WebSocket fragmented send tests passed.\n");
}

static void test_websocket_sequenced(void) {
    printf("Testing WebSocket sequenced send/receive...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Version", "13");

    cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
    assert(ws != NULL);

    char response_buf[1024];
    ssize_t nbytes = read(sv[1], response_buf, sizeof(response_buf) - 1);
    assert(nbytes > 0);
    response_buf[nbytes] = '\0';
    assert(strstr(response_buf, "101 Switching Protocols") != NULL);

    const char *message = "CWIST-sequenced-websocket-message";
    size_t msg_len = strlen(message);
    assert(cwist_websocket_send_sequenced(ws, (const uint8_t *)message, msg_len, 8) == 0);

    /* Read frames from sv[1] and verify they carry sequence chunks. */
    size_t total_payload = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t head[2];
        assert(read(sv[1], head, 2) == 2);
        assert(head[0] == 0x82); /* FIN binary, server does not mask */
        assert((head[1] & 0x80) == 0);
        uint8_t plen = head[1] & 0x7f;
        uint8_t payload[128];
        assert(read(sv[1], payload, plen) == plen);
        cwist_seq_chunk_t chunk;
        assert(cwist_seq_chunk_parse(payload, plen, &chunk));
        assert(chunk.total > 1 || chunk.seq == 1);
        total_payload += chunk.payload_len;
        if (chunk.seq == chunk.total) break;
    }
    assert(total_payload == msg_len);

    /* Now test receive_sequenced: send masked binary frames out of order. */
    cwist_seq_message_t msg;
    assert(cwist_seq_split((const uint8_t *)"hello cwist", 11, 4, &msg));
    assert(msg.count == 3);
    /* Send chunks 2, 0, 1 (out of order). */
    send_masked_binary_frame(sv[1], msg.chunks[1], msg.chunk_lens[1]);
    send_masked_binary_frame(sv[1], msg.chunks[0], msg.chunk_lens[0]);
    send_masked_binary_frame(sv[1], msg.chunks[2], msg.chunk_lens[2]);

    size_t received_len = 0;
    uint8_t *received = cwist_websocket_receive_sequenced(ws, &received_len);
    assert(received != NULL);
    assert(received_len == 11);
    assert(memcmp(received, "hello cwist", 11) == 0);

    cwist_free(received);
    cwist_seq_message_free(&msg);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);
    printf("WebSocket sequenced test passed.\n");
}

static void test_frame_validation(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    cwist_http_request *req;
    cwist_websocket *ws = upgrade_for_frames(sv, &req);
    uint8_t oversized[126] = {0};
    const uint8_t controls[] = {CWIST_WS_FRAME_CLOSE, CWIST_WS_FRAME_PING, CWIST_WS_FRAME_PONG};
    assert(cwist_websocket_send(ws, 0x03, NULL, 0) == -1);
    assert(cwist_websocket_send(ws, 0x0B, NULL, 0) == -1);
    for (size_t i = 0; i < sizeof(controls); i++)
        assert(cwist_websocket_send(ws, controls[i], oversized, sizeof(oversized)) == -1);
    int flags = fcntl(sv[1], F_GETFL, 0);
    assert(flags >= 0 && fcntl(sv[1], F_SETFL, flags | O_NONBLOCK) == 0);
    uint8_t byte;
    assert(recv(sv[1], &byte, 1, 0) == -1 && errno == EAGAIN);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);

    const uint8_t bad999[] = {0x03, 0xE7};
    const uint8_t bad1005[] = {0x03, 0xED};
    const uint8_t bad_utf8[] = {0x03, 0xE8, 0xC0, 0xAF};
    const uint8_t *invalid[] = {bad999, bad1005, bad_utf8};
    const size_t invalid_len[] = {sizeof(bad999), sizeof(bad1005), sizeof(bad_utf8)};
    for (size_t i = 0; i < 3; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ws = upgrade_for_frames(sv, &req);
        send_masked_frame(sv[1], 0x88, invalid[i], invalid_len[i]);
        assert(cwist_websocket_receive(ws) == NULL);
        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }
    for (size_t i = 0; i < 2; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ws = upgrade_for_frames(sv, &req);
        send_masked_extended_frame(sv[1], 0x81, (const uint8_t *)"x", 1, i == 0 ? 126 : 127);
        assert(cwist_websocket_receive(ws) == NULL);
        uint8_t close_frame[4];
        assert(read(sv[1], close_frame, sizeof(close_frame)) == (ssize_t)sizeof(close_frame));
        const uint8_t expected[] = {0x88, 0x02, 0x03, 0xEA};
        assert(memcmp(close_frame, expected, sizeof(expected)) == 0);
        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    const uint8_t bad_text[] = {0xC0, 0xAF};
    for (size_t fragmented = 0; fragmented < 2; fragmented++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ws = upgrade_for_frames(sv, &req);
        if (fragmented) {
            send_masked_frame(sv[1], 0x01, bad_text, 1);
            send_masked_frame(sv[1], 0x80, bad_text + 1, 1);
        } else {
            send_masked_frame(sv[1], 0x81, bad_text, sizeof(bad_text));
        }
        assert(cwist_websocket_receive(ws) == NULL);
        uint8_t close_frame[4];
        assert(read(sv[1], close_frame, sizeof(close_frame)) == (ssize_t)sizeof(close_frame));
        const uint8_t expected[] = {0x88, 0x02, 0x03, 0xEF};
        assert(memcmp(close_frame, expected, sizeof(expected)) == 0);
        cwist_websocket_destroy(ws);
        cwist_http_request_destroy(req);
        close(sv[0]);
        close(sv[1]);
    }

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ws = upgrade_for_frames(sv, &req);
    uint8_t boundary[126];
    memset(boundary, 'v', sizeof(boundary));
    send_masked_extended_frame(sv[1], 0x81, boundary, sizeof(boundary), 126);
    cwist_ws_frame *frame = cwist_websocket_receive(ws);
    assert(frame && frame->opcode == CWIST_WS_FRAME_TEXT && frame->payload_len == sizeof(boundary));
    assert(memcmp(frame->payload, boundary, sizeof(boundary)) == 0);
    cwist_websocket_frame_destroy(frame);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);
    puts("WebSocket frame validation tests passed.");
}

int main() {
    test_handshake_key_generation();
    test_handshake_rejects_bad_version();
    test_handshake_validation();
    test_websocket_sequenced();
    test_orphan_continuation_rejected();
    test_reserved_opcodes_rejected();
    test_fragmentation_state();
    test_websocket_send_fragments();
    test_frame_validation();
    return 0;
}
