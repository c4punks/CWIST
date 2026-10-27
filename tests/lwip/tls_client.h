/* In-process TLS 1.2 client for the lwIP TLS tests: mbedTLS over a
 * loop_client pcb (loopback netif, fake clock), verifying the server against
 * one CA certificate for "localhost". Shared by tests/test_lwip_tls.c (host)
 * and tests/baremetal/hello_tls.c (Cortex-M under qemu-arm). The caller
 * provides cwist_port_entropy(), which seeds the client's DRBG too. */
#ifndef CWIST_TEST_TLS_CLIENT_H
#define CWIST_TEST_TLS_CLIENT_H

#include "loop_client.h"

#include "mbedtls/ssl.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct tls_client {
    loop_client tcp;
    size_t tcp_off; /* ciphertext consumed from tcp.rx */
    mbedtls_ssl_context ssl;
    char *plain;    /* decrypted bytes, NUL-terminated; caller's buffer */
    size_t plain_len;
    size_t plain_cap;
    bool close_notify;
} tls_client;

/** Parse the CA (PEM, NUL counted in @p ca_len) and set up the shared client config. */
void tls_client_global_init(const unsigned char *ca, size_t ca_len);
void tls_client_global_free(void);

/** TCP connect + handshake into @p plain / @p cap. @return mbedTLS result (0 = success). */
int tls_client_connect(tls_client *c, uint16_t port, char *plain, size_t cap);
/** Write all of @p data, running the loop as needed. @return false when it did not complete. */
bool tls_client_write(tls_client *c, const char *data, size_t len);
/** Decrypt whatever has arrived into plain; notes close_notify. */
void tls_client_drain(tls_client *c);
/** Complete Content-Length-framed responses in plain (100 Continue excluded). */
int tls_client_responses(const tls_client *c);
/** Run until @p n responses are complete or @p max_ms elapse. */
bool tls_client_wait(tls_client *c, int n, uint32_t max_ms);
void tls_client_close(tls_client *c);

#endif /* CWIST_TEST_TLS_CLIENT_H */
