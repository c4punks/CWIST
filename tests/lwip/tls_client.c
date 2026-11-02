/* See tls_client.h. */
#include "tls_client.h"

#include <cwist/sys/port.h>

#include "lwip/tcp.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

#include <stdlib.h>
#include <string.h>

static mbedtls_ssl_config g_conf;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_ca;

static int cli_entropy(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    return cwist_port_entropy(out, len) ? -1 : 0;
}

static int cli_send(void *ctx, const unsigned char *buf, size_t len) {
    tls_client *c = ctx;
    if (!c->tcp.pcb) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    size_t room = tcp_sndbuf(c->tcp.pcb);
    if (room == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    size_t n = len < room ? len : room;
    if (tcp_write(c->tcp.pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    tcp_output(c->tcp.pcb);
    return (int)n;
}

static int cli_recv(void *ctx, unsigned char *buf, size_t len) {
    tls_client *c = ctx;
    size_t avail = c->tcp.rx_len - c->tcp_off;
    if (avail == 0) return (c->tcp.closed || c->tcp.errored) ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = len < avail ? len : avail;
    memcpy(buf, c->tcp.rx + c->tcp_off, n);
    c->tcp_off += n;
    return (int)n;
}

void tls_client_global_init(const unsigned char *ca, size_t ca_len) {
    static const unsigned char pers[] = "test-client";
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_ca);
    if (mbedtls_ctr_drbg_seed(&g_drbg, cli_entropy, NULL, pers, sizeof pers - 1) != 0 ||
        mbedtls_x509_crt_parse(&g_ca, ca, ca_len) != 0 ||
        mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        abort();
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
}

void tls_client_global_free(void) {
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
}

int tls_client_connect(tls_client *c, uint16_t port, char *plain, size_t cap) {
    memset(c, 0, sizeof(*c));
    c->plain = plain;
    c->plain_cap = cap;
    plain[0] = '\0';
    if (!loop_connect(&c->tcp, port)) return -1;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &g_conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, "localhost") != 0)
        return -1;
    mbedtls_ssl_set_bio(&c->ssl, c, cli_send, cli_recv, NULL);
    int r = -1;
    for (int i = 0; i < 2000; i++) {
        r = mbedtls_ssl_handshake(&c->ssl);
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return r;
        loop_run(5);
    }
    return r;
}

bool tls_client_write(tls_client *c, const char *data, size_t len) {
    size_t off = 0;
    for (int i = 0; i < 4000 && off < len; i++) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off, len - off);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_WANT_READ) return false;
        loop_run(5);
    }
    loop_run(20);
    return off == len;
}

void tls_client_drain(tls_client *c) {
    while (c->plain_len + 1 < c->plain_cap) {
        int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)c->plain + c->plain_len,
                                 c->plain_cap - 1 - c->plain_len);
        if (r > 0) {
            c->plain_len += (size_t)r;
            c->plain[c->plain_len] = '\0';
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) c->close_notify = true;
        return;
    }
}

int tls_client_responses(const tls_client *c) {
    int n = 0;
    const char *p = c->plain, *end = c->plain + c->plain_len;
    while (p < end) {
        const char *head = strstr(p, "\r\n\r\n");
        if (!head) break;
        head += 4;
        if (strncmp(p, "HTTP/1.1 100 ", 13) == 0) {
            p = head;
            continue;
        }
        const char *h = strstr(p, "Content-Length: ");
        size_t cl = (h && h < head) ? strtoul(h + 16, NULL, 10) : 0;
        if ((size_t)(end - head) < cl) break;
        n++;
        p = head + cl;
    }
    return n;
}

bool tls_client_wait(tls_client *c, int n, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms; t += 10) {
        tls_client_drain(c);
        if (tls_client_responses(c) >= n) return true;
        loop_run(10);
    }
    return false;
}

void tls_client_close(tls_client *c) {
    mbedtls_ssl_free(&c->ssl);
    loop_close(&c->tcp);
}
