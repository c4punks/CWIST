/* Bare-metal HTTPS smoke (issue #447 Phase 4): the lwIP adapter with
 * CWIST_LWIP_TLS, lwIP and mbedTLS 3.6 on Cortex-M7, against an mbedTLS
 * client over the loopback netif. Checks the handshake (certificate
 * verified for "localhost"), a keep-alive GET, a POST body, an 8 KB
 * response through 4 KB records and a 2-segment send window, and
 * close_notify on Connection: close. Reports heap/stack high-water marks
 * for the whole process (server and client sessions together). Run with
 * `make baremetal-smoke`. */
#include <cwist/app.h>
#include <cwist/sys/port/lwip.h>

#include "loop_client.h"
#include "test_tls_cert.h"
#include "tls_client.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

extern void board_log(const char *s);
extern size_t board_heap_high_water(void);
extern void board_stack_paint(void);
extern size_t board_stack_used(void);
extern unsigned board_entropy_calls;
extern void board_alloc_begin(void);
extern size_t board_alloc_end(void);

static char g_big[8192];
static char g_echo[32];
static char g_plain[12288];

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello over tls", 14);
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    int n = snprintf(g_echo, sizeof g_echo, "len=%u", (unsigned)(req->body ? req->body->size : 0));
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

static int check(const char *what, int ok) {
    board_log(ok ? "PASS " : "FAIL ");
    board_log(what);
    board_log("\n");
    return !ok;
}

int main(void) {
    board_stack_paint();
    for (size_t i = 0; i < sizeof g_big; i++) g_big[i] = (char)('a' + i % 26);
    loop_init();

    cwist_app *app = cwist_app_create();
    if (!app) return check("cwist_app_create", 0);
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_post(app, "/echo", echo_handler);
    cwist_app_get(app, "/big", big_handler);
    cwist_app_get(app, "/close", close_handler);

    unsigned seeded = board_entropy_calls;
    cwist_lwip_tls *tls = cwist_lwip_tls_create(
        (const unsigned char *)CWIST_TEST_TLS_CERT_PEM, sizeof CWIST_TEST_TLS_CERT_PEM,
        (const unsigned char *)CWIST_TEST_TLS_KEY_PEM, sizeof CWIST_TEST_TLS_KEY_PEM);
    int fail = check("TLS context (cert + key + DRBG seeded from cwist_port_entropy)",
                     tls != NULL && board_entropy_calls > seeded);
    if (!tls) return fail;

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.port = 443;
    cfg.max_body_bytes = 1024;
    cfg.tls = tls;
    cwist_lwip_server *srv = cwist_lwip_listen(app, &cfg);
    fail |= check("listen on :443", srv != NULL);
    if (!srv) return fail;

    tls_client_global_init((const unsigned char *)CWIST_TEST_TLS_CERT_PEM, sizeof CWIST_TEST_TLS_CERT_PEM);
    tls_client c;
    size_t in_use_before = (size_t)mallinfo().uordblks;
    int r = tls_client_connect(&c, 443, g_plain, sizeof g_plain);
    size_t sessions = (size_t)mallinfo().uordblks - in_use_before;
    fail |= check("handshake, certificate verified for localhost",
                  r == 0 && mbedtls_ssl_get_verify_result(&c.ssl) == 0);
    if (r != 0) {
        char buf[48];
        snprintf(buf, sizeof buf, "handshake error -0x%04x\n", (unsigned)-r);
        board_log(buf);
        return 1;
    }
    fail |= check("TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256",
                  strcmp(mbedtls_ssl_get_ciphersuite(&c.ssl), "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256") == 0);

    static const char get[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    tls_client_write(&c, get, sizeof get - 1);
    fail |= check("GET /hello over TLS, keep-alive",
                  tls_client_wait(&c, 1, 3000) && strstr(c.plain, "hello over tls") &&
                      strstr(c.plain, "Connection: keep-alive"));

    static const char post[] = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\nabcdef";
    tls_client_write(&c, post, sizeof post - 1);
    fail |= check("POST body in the same session", tls_client_wait(&c, 2, 3000) && strstr(c.plain, "len=6"));

    static const char big[] = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t before = c.plain_len;
    tls_client_write(&c, big, sizeof big - 1);
    int ok = tls_client_wait(&c, 3, 10000);
    const char *body = ok ? strstr(c.plain + before, "\r\n\r\n") : NULL;
    fail |= check("8 KB response through 4 KB records",
                  body && (size_t)(c.plain + c.plain_len - (body + 4)) == sizeof g_big &&
                      memcmp(body + 4, g_big, sizeof g_big) == 0);

    board_alloc_begin();
    tls_client_write(&c, get, sizeof get - 1);
    int steady_ok = tls_client_wait(&c, 4, 3000);
    size_t steady_allocs = board_alloc_end();
    fail |= check("zero allocations per steady TLS response", steady_ok && steady_allocs == 0);

    static const char bye[] = "GET /close HTTP/1.1\r\nHost: localhost\r\n\r\n";
    tls_client_write(&c, bye, sizeof bye - 1);
    tls_client_wait(&c, 5, 3000);
    for (int i = 0; i < 100 && !c.close_notify; i++) {
        loop_run(10);
        tls_client_drain(&c);
    }
    fail |= check("Connection: close sends close_notify", c.close_notify && strstr(c.plain, "bye"));
    tls_client_close(&c);

    loop_run(200);
    fail |= check("all connections released", cwist_lwip_active_connections(srv) == 0);
    cwist_lwip_close(srv);
    cwist_lwip_tls_destroy(tls);
    tls_client_global_free();
    cwist_app_destroy(app);

    char buf[112];
    snprintf(buf, sizeof buf, "steady allocs per request: %u\n", (unsigned)steady_allocs);
    board_log(buf);
    snprintf(buf, sizeof buf, "mbedtls_ssl_context: %u bytes, records: in %u + out %u bytes\n",
             (unsigned)sizeof(mbedtls_ssl_context), (unsigned)MBEDTLS_SSL_IN_CONTENT_LEN,
             (unsigned)MBEDTLS_SSL_OUT_CONTENT_LEN);
    board_log(buf);
    snprintf(buf, sizeof buf, "heap in use by both TLS sessions after the handshake: %u bytes\n",
             (unsigned)sessions);
    board_log(buf);
    snprintf(buf, sizeof buf, "heap high water: %u bytes, stack used: %u bytes\n",
             (unsigned)board_heap_high_water(), (unsigned)board_stack_used());
    board_log(buf);
    board_log(fail ? "baremetal-tls-smoke: FAIL\n" : "baremetal-tls-smoke: PASS\n");
    return fail;
}
