#include "../src/net/http/http3_client.c"

#include <assert.h>
#include <stdio.h>

int main(void) {
    cwist_http3_client client;
    memset(&client, 0, sizeof(client));
    pthread_mutex_init(&client.dgram_mtx, NULL);

    char out[8];
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == -1);
    assert(errno == EAGAIN);

    h3c_dgram_enqueue(&client, "first", 5);
    h3c_dgram_enqueue(&client, "second", 6);
    assert(client.in_dgram.count == 2);

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
    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == 6);
    assert(memcmp(out, "second", 6) == 0);
    assert(client.in_dgram.count == 0);
    assert(client.in_dgram.head == NULL && client.in_dgram.tail == NULL);

    for (uint8_t i = 0; i < H3C_DGRAM_QUEUE_LIMIT; i++) {
        h3c_dgram_enqueue(&client, &i, 1);
        assert(client.in_dgram.count == (size_t)i + 1);
    }
    uint8_t overflow = 0xFF;
    h3c_dgram_enqueue(&client, &overflow, 1);
    assert(client.in_dgram.count == H3C_DGRAM_QUEUE_LIMIT);
    for (uint8_t i = 0; i < H3C_DGRAM_QUEUE_LIMIT; i++) {
        assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == 1);
        assert(out[0] == (char)i);
    }
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == -1);
    assert(errno == EAGAIN);

    pthread_mutex_destroy(&client.dgram_mtx);
    printf("test_http3_client_datagram: ok\n");
    return 0;
}
