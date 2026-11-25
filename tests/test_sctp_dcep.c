#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define cwist_sctp_global_init test_sctp_global_init
#define cwist_sctp_tick test_sctp_tick
#define cwist_sctp_conn_open test_sctp_conn_open
#define cwist_sctp_conn_close test_sctp_conn_close
#define cwist_sctp_conn_input test_sctp_conn_input
#define cwist_sctp_rx_partial_append test_sctp_rx_partial_append
#define cwist_sctp_conn_drain test_sctp_conn_drain
#define cwist_sctp_assoc_established test_sctp_assoc_established
#define cwist_sctp_send test_sctp_send
#include "../src/net/webrtc/sctp.c"

static int open_calls;
static int string_calls;
static int binary_calls;

static void opened(struct cwist_webrtc_conn *conn, uint16_t channel, const char *label, void *user) {
    (void)conn;
    (void)channel;
    (void)label;
    (void)user;
    open_calls++;
}

static void message(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, cwist_webrtc_data_type type, void *user) {
    (void)conn;
    (void)channel;
    (void)data;
    (void)len;
    (void)user;
    if (type == CWIST_WEBRTC_DATA_STRING)
        string_calls++;
    else
        binary_calls++;
}

int main(void) {
    cwist_webrtc_ctx *ctx = calloc(1, sizeof(*ctx));
    struct cwist_webrtc_conn conn = {.ctx = ctx};
    assert(ctx != NULL);
    ctx->ch_cb = opened;
    ctx->msg_cb = message;

    uint8_t truncated[12] = {DCEP_OPEN};
    truncated[9] = 1;
    dispatch_message(&conn, 1, PPID_DCEP, truncated, sizeof(truncated));
    truncated[11] = 1;
    dispatch_message(&conn, 1, PPID_DCEP, truncated, sizeof(truncated));
    assert(open_calls == 0);

    uint8_t empty = 0;
    dispatch_message(&conn, 1, PPID_STRING_EMPTY, &empty, 1);
    dispatch_message(&conn, 1, PPID_BINARY_EMPTY, &empty, 1);
    assert(string_calls == 1 && binary_calls == 1);
    uint8_t nonempty = 1;
    dispatch_message(&conn, 1, PPID_STRING_EMPTY, &nonempty, 1);
    uint8_t extra[2] = {0};
    dispatch_message(&conn, 1, PPID_BINARY_EMPTY, extra, sizeof(extra));
    assert(string_calls == 1 && binary_calls == 1);

    free(conn.ch_bits);
    free(conn.rx_partial);
    free(ctx);
    puts("DCEP structural validation: ok");
    return 0;
}
