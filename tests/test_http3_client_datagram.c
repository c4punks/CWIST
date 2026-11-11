#include "../src/net/http/http3_client.c"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    assert(h3c_timeout_to_poll_us(-1) == 1000);
    assert(h3c_timeout_to_poll_us(0) == 1000);
    assert(h3c_timeout_to_poll_us(1) == 1000);
    assert(h3c_timeout_to_poll_us(999) == 999000);
    assert(h3c_timeout_to_poll_us(1000) == 1000000);
    assert(h3c_timeout_to_poll_us(INT_MAX) == 1000000);

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
    printf("test_http3_client_datagram: ok\n");
    return 0;
}
