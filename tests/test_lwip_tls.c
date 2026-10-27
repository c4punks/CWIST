/* HTTPS over the lwIP adapter (src/sys/port/lwip.c with CWIST_LWIP_TLS,
 * issue #447 Phase 4) on the host. The server is the adapter on vendored
 * lwIP + mbedTLS 3.6; the client is an mbedTLS TLS 1.2 client driving a raw
 * lwIP pcb over the loopback netif, verifying the server certificate
 * against the test CA for "localhost". Covers context creation failures
 * (bad certificate, mismatched key, failing entropy hook), the handshake,
 * keep-alive and pipelining inside one session, a body split across
 * records with Expect: 100-continue, a response larger than both the send
 * window and one TLS record, a framing error, Connection: close with
 * close_notify, and a plaintext client hitting the TLS port. */
#include <cwist/app.h>
#include <cwist/sys/port.h>
#include <cwist/sys/port/lwip.h>

#include "loop_client.h"
#include "test_tls_cert.h"
#include "tls_client.h"

#include "mbedtls/error.h"
#include "mbedtls/ssl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- firmware hooks (the TRNG is a counter-driven xorshift here) ----------- */

static unsigned g_entropy_calls;
static int g_entropy_fail;
static uint32_t g_rng = 0x2545F491u;

int cwist_port_entropy(void *buf, size_t len) {
    g_entropy_calls++;
    if (g_entropy_fail) return -1;
    uint8_t *out = buf;
    for (size_t i = 0; i < len; i++) {
        g_rng ^= g_rng << 13;
        g_rng ^= g_rng >> 17;
        g_rng ^= g_rng << 5;
        out[i] = (uint8_t)g_rng;
    }
    return 0;
}

uint64_t cwist_port_monotonic_ns(void) { return (uint64_t)loop_now() * 1000000u; }

/* --- app --------------------------------------------------------------------- */

static char g_big[40000];
static char g_echo[32];

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello over tls", 14);
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    int n = snprintf(g_echo, sizeof g_echo, "len=%zu", req->body ? req->body->size : 0);
    cwist_http_response_set_body_ptr(res, g_echo, (size_t)n);
}

static void big_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, g_big, sizeof g_big);
}

static void close_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->keep_alive = false;
    cwist_http_response_set_body_ptr(res, "bye", 3);
}

static char g_plain[65536];

static void tls_client_write_ok(tls_client *c, const char *data, size_t len) {
    assert(tls_client_write(c, data, len));
}

/* --- tests ---------------------------------------------------------------------- */

static cwist_app *g_app;
static cwist_lwip_tls *g_tls;

static void test_create_failures(void) {
    const unsigned char *cert = (const unsigned char *)CWIST_TEST_TLS_CERT_PEM;
    const unsigned char *key = (const unsigned char *)CWIST_TEST_TLS_KEY_PEM;
    assert(!cwist_lwip_tls_create(NULL, 0, key, sizeof CWIST_TEST_TLS_KEY_PEM));
    static const char junk[] = "-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n";
    assert(!cwist_lwip_tls_create((const unsigned char *)junk, sizeof junk, key,
                                  sizeof CWIST_TEST_TLS_KEY_PEM));
    assert(!cwist_lwip_tls_create(cert, sizeof CWIST_TEST_TLS_CERT_PEM,
                                  (const unsigned char *)CWIST_TEST_TLS_OTHER_KEY_PEM,
                                  sizeof CWIST_TEST_TLS_OTHER_KEY_PEM));
    /* fail closed: no context without entropy */
    g_entropy_fail = 1;
    unsigned before = g_entropy_calls;
    assert(!cwist_lwip_tls_create(cert, sizeof CWIST_TEST_TLS_CERT_PEM, key,
                                  sizeof CWIST_TEST_TLS_KEY_PEM));
    assert(g_entropy_calls > before);
    g_entropy_fail = 0;
    cwist_lwip_tls_destroy(NULL);
}

static const char GET_HELLO[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";

static void test_session(cwist_lwip_server *srv) {
    tls_client c;
    int r = tls_client_connect(&c, 443, g_plain, sizeof g_plain);
    if (r != 0) {
        char err[128];
        mbedtls_strerror(r, err, sizeof err);
        fprintf(stderr, "handshake failed: -0x%04x %s\n", (unsigned)-r, err);
        assert(0);
    }
    assert(mbedtls_ssl_get_verify_result(&c.ssl) == 0); /* chain + hostname verified */
    assert(strcmp(mbedtls_ssl_get_ciphersuite(&c.ssl), "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256") == 0);
    assert(cwist_lwip_active_connections(srv) == 1);

    tls_client_write_ok(&c, GET_HELLO, sizeof GET_HELLO - 1);
    assert(tls_client_wait(&c, 1, 2000));
    assert(strstr(c.plain, "HTTP/1.1 200") && strstr(c.plain, "Connection: keep-alive"));
    assert(strstr(c.plain, "\r\n\r\nhello over tls"));

    /* two requests in one record, answered in order in the same session */
    static const char two[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n"
                              "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc";
    tls_client_write_ok(&c, two, sizeof two - 1);
    assert(tls_client_wait(&c, 3, 2000));
    assert(strstr(c.plain, "len=3"));

    /* body in separate records after a 100 Continue */
    static const char head[] = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n"
                               "Expect: 100-continue\r\n\r\n";
    size_t before = c.plain_len;
    tls_client_write_ok(&c, head, sizeof head - 1);
    loop_run(100);
    tls_client_drain(&c);
    assert(strstr(c.plain + before, "HTTP/1.1 100 Continue\r\n\r\n"));
    tls_client_write_ok(&c, "01234", 5);
    tls_client_write_ok(&c, "56789", 5);
    assert(tls_client_wait(&c, 4, 2000));
    assert(strstr(c.plain + before, "len=10"));

    /* 40 KB: many 4 KB records through a 2-segment send window */
    static const char big[] = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
    before = c.plain_len;
    tls_client_write_ok(&c, big, sizeof big - 1);
    assert(tls_client_wait(&c, 5, 10000));
    const char *body = strstr(c.plain + before, "\r\n\r\n") + 4;
    assert((size_t)(c.plain + c.plain_len - body) == sizeof g_big);
    assert(memcmp(body, g_big, sizeof g_big) == 0);

    /* Connection: close -> close_notify, then FIN */
    static const char bye[] = "GET /close HTTP/1.1\r\nHost: localhost\r\n\r\n";
    tls_client_write_ok(&c, bye, sizeof bye - 1);
    assert(tls_client_wait(&c, 6, 2000));
    assert(strstr(c.plain, "\r\n\r\nbye"));
    for (int i = 0; i < 100 && !c.close_notify; i++) {
        loop_run(10);
        tls_client_drain(&c);
    }
    assert(c.close_notify);
    assert(loop_wait_closed(&c.tcp, 2000));
    tls_client_close(&c);
    loop_run(100);
    assert(cwist_lwip_active_connections(srv) == 0);
}

static void test_framing_error_over_tls(cwist_lwip_server *srv) {
    tls_client c;
    assert(tls_client_connect(&c, 443, g_plain, sizeof g_plain) == 0);
    static const char huge[] = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999999\r\n\r\n";
    tls_client_write_ok(&c, huge, sizeof huge - 1);
    assert(tls_client_wait(&c, 1, 2000));
    assert(strstr(c.plain, "HTTP/1.1 413") && strstr(c.plain, "Connection: close"));
    assert(loop_wait_closed(&c.tcp, 2000));
    tls_client_close(&c);
    loop_run(100);
    assert(cwist_lwip_active_connections(srv) == 0);
}

static void test_plaintext_client_rejected(cwist_lwip_server *srv) {
    loop_client p;
    assert(loop_connect(&p, 443));
    loop_send(&p, GET_HELLO, sizeof GET_HELLO - 1);
    assert(loop_wait_closed(&p, 2000)); /* not a ClientHello: the handshake fails */
    assert(!p.rx || !strstr(p.rx, "HTTP/1.1")); /* nothing served in the clear */
    loop_close(&p);
    loop_run(100);
    assert(cwist_lwip_active_connections(srv) == 0);
}

int main(void) {
    for (size_t i = 0; i < sizeof g_big; i++) g_big[i] = (char)('A' + i % 26);
    loop_init();
    tls_client_global_init((const unsigned char *)CWIST_TEST_TLS_CERT_PEM, sizeof CWIST_TEST_TLS_CERT_PEM);
    g_app = cwist_app_create();
    assert(g_app);
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_app_get(g_app, "/big", big_handler);
    cwist_app_get(g_app, "/close", close_handler);

    test_create_failures();

    unsigned before = g_entropy_calls;
    g_tls = cwist_lwip_tls_create((const unsigned char *)CWIST_TEST_TLS_CERT_PEM,
                                  sizeof CWIST_TEST_TLS_CERT_PEM,
                                  (const unsigned char *)CWIST_TEST_TLS_KEY_PEM,
                                  sizeof CWIST_TEST_TLS_KEY_PEM);
    assert(g_tls);
    assert(g_entropy_calls > before); /* the DRBG was seeded from the hook */

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    assert(cfg.tls == NULL);
    cfg.port = 443;
    cfg.max_body_bytes = 1024;
    cfg.tls = g_tls;
    cwist_lwip_server *srv = cwist_lwip_listen(g_app, &cfg);
    assert(srv);

    test_session(srv);
    test_framing_error_over_tls(srv);
    test_plaintext_client_rejected(srv);

    cwist_lwip_close(srv);
    loop_run(50);
    cwist_lwip_tls_destroy(g_tls);
    cwist_app_destroy(g_app);
    tls_client_global_free();
    printf("test_lwip_tls: all passed\n");
    return 0;
}
