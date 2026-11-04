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

    static const uint8_t payload[] = {'1', '2', '3', '4', '5', '6', '7', '8'};
    client.in_dgram.data = malloc(sizeof(payload));
    assert(client.in_dgram.data != NULL);
    memcpy(client.in_dgram.data, payload, sizeof(payload));
    client.in_dgram.len = sizeof(payload);
    client.in_dgram.ready = 1;

    char small[4] = {0};
    errno = 0;
    assert(cwist_http3_client_recv_datagram(&client, small, sizeof(small)) == -1);
    assert(errno == EMSGSIZE);
    assert(client.in_dgram.ready == 1);
    assert(client.in_dgram.len == sizeof(payload));
    assert(memcmp(client.in_dgram.data, payload, sizeof(payload)) == 0);
    assert(small[0] == 0 && small[1] == 0 && small[2] == 0 && small[3] == 0);

    assert(cwist_http3_client_recv_datagram(&client, out, sizeof(out)) == (ssize_t)sizeof(out));
    assert(memcmp(out, payload, sizeof(payload)) == 0);
    assert(client.in_dgram.ready == 0);
    assert(client.in_dgram.data == NULL);
    assert(client.in_dgram.len == 0);

    pthread_mutex_destroy(&client.dgram_mtx);
    printf("test_http3_client_datagram: ok\n");
    return 0;
}
