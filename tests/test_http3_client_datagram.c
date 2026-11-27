#include "../src/net/http/http3_client.c"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    assert(h3c_regular_name_valid("content-type", 12));
    assert(!h3c_regular_name_valid("Content-Type", 12));
    assert(!h3c_regular_name_valid(":status", 7));
    assert(!h3c_regular_name_valid("bad name", 8));
    assert(h3c_field_value_valid("value\twith-tab", 14));
    assert(!h3c_field_value_valid("bad\001value", 9));
    size_t parsed_length = 0;
    assert(h3c_parse_content_length("0", 1, &parsed_length) == 0 && parsed_length == 0);
    assert(h3c_parse_content_length("1024", 4, &parsed_length) == 0 && parsed_length == 1024);
    assert(h3c_parse_content_length("-1", 2, &parsed_length) == -1);
    assert(h3c_parse_content_length("18446744073709551616", 20, &parsed_length) == -1);

    assert(h3c_timeout_to_poll_us(-1) == 1000);
    assert(h3c_timeout_to_poll_us(0) == 1000);
    assert(h3c_timeout_to_poll_us(1) == 1000);
    assert(h3c_timeout_to_poll_us(999) == 999000);
    assert(h3c_timeout_to_poll_us(1000) == 1000000);
    assert(h3c_timeout_to_poll_us(INT_MAX) == 1000000);
    assert(h3c_retry_backoff_ms(1000, 1) == 1000);
    assert(h3c_retry_backoff_ms(1000, 2) == 2000);
    assert(h3c_retry_backoff_ms(1000, 5) == 16000);
    assert(h3c_retry_backoff_ms(1000, 6) == 30000);
    assert(h3c_retry_backoff_ms(1, 31) == 30000);
    assert(h3c_retry_backoff_ms(INT_MAX, 1) == 30000);

    cwist_http3_client client;
    memset(&client, 0, sizeof(client));
    pthread_mutex_init(&client.dgram_mtx, NULL);

    cwist_http3_datagram_stats stats = {0};
    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 0);
    assert(stats.dequeued == 0);
    assert(stats.dropped_full == 0);
    assert(stats.dropped_alloc == 0);
    assert(stats.queue_depth == 0);

    char out[8];
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == -1);
    assert(errno == EAGAIN);

    h3c_dgram_enqueue(&client, "first", 5);
    h3c_dgram_enqueue(&client, "second", 6);
    assert(client.in_dgram.count == 2);

    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 2);
    assert(stats.dequeued == 0);
    assert(stats.queue_depth == 2);

    char small[4] = {0};
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, small, sizeof(small)) == -1);
    assert(errno == EMSGSIZE);
    assert(client.in_dgram.count == 2);
    assert(client.in_dgram.head->len == 5);
    assert(memcmp(client.in_dgram.head->data, "first", 5) == 0);
    assert(small[0] == 0 && small[1] == 0 && small[2] == 0 && small[3] == 0);

    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == 5);
    assert(memcmp(out, "first", 5) == 0);

    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 2);
    assert(stats.dequeued == 1);
    assert(stats.queue_depth == 1);

    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == 6);
    assert(memcmp(out, "second", 6) == 0);
    assert(client.in_dgram.count == 0);
    assert(client.in_dgram.head == NULL && client.in_dgram.tail == NULL);

    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 2);
    assert(stats.dequeued == 2);
    assert(stats.queue_depth == 0);

    for (uint8_t i = 0; i < H3C_DGRAM_QUEUE_LIMIT; i++) {
        h3c_dgram_enqueue(&client, &i, 1);
        assert(client.in_dgram.count == (size_t)i + 1);
    }
    uint8_t overflow = 0xFF;
    h3c_dgram_enqueue(&client, &overflow, 1);
    assert(client.in_dgram.count == H3C_DGRAM_QUEUE_LIMIT);

    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 2 + H3C_DGRAM_QUEUE_LIMIT);
    assert(stats.dequeued == 2);
    assert(stats.dropped_full == 1);
    assert(stats.queue_depth == H3C_DGRAM_QUEUE_LIMIT);

    for (uint8_t i = 0; i < H3C_DGRAM_QUEUE_LIMIT; i++) {
        assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == 1);
        assert(out[0] == (char)i);
    }
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == -1);
    assert(errno == EAGAIN);

    cwist_http3_client_get_datagram_stats(&client, &stats);
    assert(stats.enqueued == 2 + H3C_DGRAM_QUEUE_LIMIT);
    assert(stats.dequeued == 2 + H3C_DGRAM_QUEUE_LIMIT);
    assert(stats.dropped_full == 1);
    assert(stats.dropped_alloc == 0);
    assert(stats.queue_depth == 0);

    pthread_mutex_destroy(&client.dgram_mtx);

    /* Test checked configuration APIs (Issue #603) */
    /* 1. NULL client */
    cwist_error_t err_null = cwist_http3_client_enable_datagrams_checked(NULL, 1);
    assert(!cwist_error_is_ok(&err_null));
    assert(err_null.error.err_i16 == EINVAL);

    err_null = cwist_http3_client_enable_datagram_channel_checked(NULL, 1);
    assert(!cwist_error_is_ok(&err_null));
#ifdef CWIST_WEBTRANSPORT_CLIENT
    assert(err_null.error.err_i16 == EINVAL);
#else
    assert(err_null.error.err_i16 == ENOSYS);
#endif

    /* 2. Active-connection rejection */
    cwist_http3_client mock_client;
    memset(&mock_client, 0, sizeof(mock_client));
    mock_client.conn = (void *)0x1;

    cwist_error_t err_busy = cwist_http3_client_enable_datagrams_checked(&mock_client, 1);
    assert(!cwist_error_is_ok(&err_busy));
    assert(err_busy.error.err_i16 == EBUSY);

    err_busy = cwist_http3_client_enable_datagram_channel_checked(&mock_client, 1);
    assert(!cwist_error_is_ok(&err_busy));
#ifdef CWIST_WEBTRANSPORT_CLIENT
    assert(err_busy.error.err_i16 == EBUSY);
#else
    assert(err_busy.error.err_i16 == ENOSYS);
#endif

    /* 3. Injected engine-creation failure */
    memset(&mock_client, 0, sizeof(mock_client));
    mock_client.datagram_enabled = 0;
    g_h3c_fail_engine_new = 1;

    cwist_error_t err_oom = cwist_http3_client_enable_datagrams_checked(&mock_client, 1);
    assert(!cwist_error_is_ok(&err_oom));
    assert(err_oom.error.err_i16 == ENOMEM);
    assert(mock_client.datagram_enabled == 0);

#ifdef CWIST_WEBTRANSPORT_CLIENT
    err_oom = cwist_http3_client_enable_datagram_channel_checked(&mock_client, 1);
    assert(!cwist_error_is_ok(&err_oom));
    assert(err_oom.error.err_i16 == ENOMEM);
    assert(mock_client.dgc_enabled == 0);
#endif

    g_h3c_fail_engine_new = 0;

    /* 4. Pre-connection changes with live client */
    cwist_http3_client *live = cwist_http3_client_create();
    if (live) {
        assert(live->datagram_enabled == 0);

        /* Successful pre-connection enable */
        cwist_error_t ok_err = cwist_http3_client_enable_datagrams_checked(live, 1);
        assert(cwist_error_is_ok(&ok_err));
        assert(live->datagram_enabled == 1);

        /* Idempotent re-configuration */
        ok_err = cwist_http3_client_enable_datagrams_checked(live, 1);
        assert(cwist_error_is_ok(&ok_err));
        assert(live->datagram_enabled == 1);

        /* Successful toggle back to disabled */
        ok_err = cwist_http3_client_enable_datagrams_checked(live, 0);
        assert(cwist_error_is_ok(&ok_err));
        assert(live->datagram_enabled == 0);

        /* Compatibility void wrapper */
        cwist_http3_client_enable_datagrams(live, 1);
        assert(live->datagram_enabled == 1);

        /* Active connection rejection on live client */
        live->conn = (void *)0x1;
        cwist_error_t live_busy = cwist_http3_client_enable_datagrams_checked(live, 0);
        assert(!cwist_error_is_ok(&live_busy));
        assert(live_busy.error.err_i16 == EBUSY);
        assert(live->datagram_enabled == 1);
        live->conn = NULL;

        /* Injected failure on live client restores previous state */
        g_h3c_fail_engine_new = 1;
        cwist_error_t live_oom = cwist_http3_client_enable_datagrams_checked(live, 0);
        assert(!cwist_error_is_ok(&live_oom));
        assert(live_oom.error.err_i16 == ENOMEM);
        assert(live->datagram_enabled == 1);
        g_h3c_fail_engine_new = 0;

#ifdef CWIST_WEBTRANSPORT_CLIENT
        ok_err = cwist_http3_client_enable_datagram_channel_checked(live, 1);
        assert(cwist_error_is_ok(&ok_err));
        assert(live->dgc_enabled == 1);

        assert(cwist_http3_client_enable_datagram_channel(live, 0) == 0);
        assert(live->dgc_enabled == 0);
#endif

        cwist_http3_client_destroy(live);
    }

    printf("test_http3_client_datagram: ok\n");
    return 0;
}
