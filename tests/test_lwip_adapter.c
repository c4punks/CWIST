/* lwIP adapter (src/sys/port/lwip.c, issue #447 Phase 3) on the host: a
 * vendored lwIP (lib/lwip) in NO_SYS mode carries client and server pcbs
 * over its loopback netif, so every byte goes through real lwIP TCP. Covers
 * keep-alive, pipelining, Content-Length bodies split across segments,
 * Expect: 100-continue, the 400/413/431/501 framing errors, Connection:
 * close, responses larger than the send window, HEAD, the connection cap,
 * the idle timeout, and teardown. */
#include <cwist/app.h>
#include <cwist/sys/port/lwip.h>

#include "lwip/tcp.h"
#include "loop_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char g_big[20000];
static char g_echo[64];

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello", 5);
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

static cwist_app *g_app;
static cwist_lwip_server *g_srv;

static void start(const cwist_lwip_config *cfg) {
    g_srv = cwist_lwip_listen(g_app, cfg);
    assert(g_srv);
}

static void stop(void) {
    cwist_lwip_close(g_srv);
    g_srv = NULL;
    loop_run(50);
}

static const char GET_HELLO[] = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";

static void test_keep_alive_and_pipelining(void) {
    start(NULL);
    loop_client c;
    assert(loop_connect(&c, 80));
    loop_send(&c, GET_HELLO, sizeof GET_HELLO - 1);
    assert(loop_wait_responses(&c, 1, 2000));
    assert(strstr(c.rx, "HTTP/1.1 200") && strstr(c.rx, "Connection: keep-alive"));
    assert(strstr(c.rx, "\r\n\r\nhello"));
    assert(cwist_lwip_active_connections(g_srv) == 1);

    /* two requests in one segment: answered in order on the same connection */
    static const char two[] = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
                              "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
    loop_send(&c, two, sizeof two - 1);
    assert(loop_wait_responses(&c, 3, 2000));
    char *echo = strstr(c.rx, "len=3");
    assert(echo && echo > strstr(strstr(c.rx, "hello") + 5, "hello"));
    assert(!c.closed);
    loop_close(&c);
    stop();
}

static void test_body_split_and_expect_continue(void) {
    start(NULL);
    loop_client c;
    assert(loop_connect(&c, 80));
    static const char head[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n"
                               "Expect: 100-continue\r\n\r\n";
    loop_send(&c, head, sizeof head - 1);
    loop_run(100);
    assert(c.rx && strstr(c.rx, "HTTP/1.1 100 Continue\r\n\r\n"));
    assert(loop_response_count(&c) == 0);
    loop_send(&c, "01234", 5);
    loop_run(100);
    assert(loop_response_count(&c) == 0);
    loop_send(&c, "56789", 5);
    assert(loop_wait_responses(&c, 1, 2000));
    assert(strstr(c.rx, "len=10"));
    loop_close(&c);
    stop();
}

static void expect_error(const char *req, const char *status_line) {
    loop_client c;
    assert(loop_connect(&c, 80));
    loop_send(&c, req, strlen(req));
    assert(loop_wait_responses(&c, 1, 2000));
    if (!strstr(c.rx, status_line)) {
        fprintf(stderr, "expected %s, got:\n%s\n", status_line, c.rx);
        assert(0);
    }
    assert(strstr(c.rx, "Connection: close"));
    assert(loop_wait_closed(&c, 2000));
    loop_close(&c);
}

static void test_framing_errors(void) {
    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_header_bytes = 256;
    cfg.max_body_bytes = 64;
    start(&cfg);
    expect_error("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                 "HTTP/1.1 501");
    expect_error("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n"
                 "Transfer-Encoding: chunked\r\n\r\nab",
                 "HTTP/1.1 400");
    expect_error("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\na", "HTTP/1.1 400");
    expect_error("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
                 "HTTP/1.1 400");
    expect_error("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65\r\n\r\n", "HTTP/1.1 413");
    char big_head[400];
    int n = snprintf(big_head, sizeof big_head, "GET /hello HTTP/1.1\r\nHost: x\r\nX-Pad: ");
    memset(big_head + n, 'a', 300);
    memcpy(big_head + n + 300, "\r\n\r\n", 5);
    expect_error(big_head, "HTTP/1.1 431");
    /* no CRLFCRLF at all, already past the cap */
    big_head[n + 300] = '\0';
    expect_error(big_head, "HTTP/1.1 431");
    /* a request the parser rejects (missing Host) after framing succeeds */
    expect_error("GET /hello HTTP/1.1\r\n\r\n", "HTTP/1.1 400");
    stop();
}

static void test_close_large_and_head(void) {
    start(NULL);
    loop_client c;
    assert(loop_connect(&c, 80));
    static const char big[] = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    loop_send(&c, big, sizeof big - 1);
    assert(loop_wait_responses(&c, 1, 5000));
    char *body = strstr(c.rx, "\r\n\r\n") + 4;
    assert((size_t)(c.rx + c.rx_len - body) == sizeof g_big);
    assert(memcmp(body, g_big, sizeof g_big) == 0);

    static const char head[] = "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t before = c.rx_len;
    loop_send(&c, head, sizeof head - 1);
    loop_run(200);
    char *h = c.rx + before;
    assert(strncmp(h, "HTTP/1.1 ", 9) == 0);
    assert(strcmp(h + strlen(h) - 4, "\r\n\r\n") == 0); /* head only, no body bytes */

    static const char bye[] = "GET /close HTTP/1.1\r\nHost: x\r\n\r\n";
    loop_send(&c, bye, sizeof bye - 1);
    assert(loop_wait_closed(&c, 2000));
    assert(strstr(c.rx, "Connection: close") && strstr(c.rx, "\r\n\r\nbye"));
    loop_close(&c);
    loop_run(100);
    assert(cwist_lwip_active_connections(g_srv) == 0);

    /* HTTP/1.0 without keep-alive closes too */
    assert(loop_connect(&c, 80));
    static const char v10[] = "GET /hello HTTP/1.0\r\n\r\n";
    loop_send(&c, v10, sizeof v10 - 1);
    assert(loop_wait_closed(&c, 2000));
    assert(strstr(c.rx, "\r\n\r\nhello"));
    loop_close(&c);
    stop();
}

static void test_conn_cap_idle_and_teardown(void) {
    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_conns = 2;
    cfg.idle_timeout_ms = 3000;
    start(&cfg);
    loop_client a, b, extra;
    assert(loop_connect(&a, 80));
    assert(loop_connect(&b, 80));
    loop_run(50);
    assert(cwist_lwip_active_connections(g_srv) == 2);
    loop_connect(&extra, 80); /* accepted by TCP, then aborted by the adapter */
    assert(loop_wait_closed(&extra, 2000) && extra.errored);
    assert(cwist_lwip_active_connections(g_srv) == 2);
    loop_close(&extra);

    /* traffic on a keeps it alive while b idles out */
    for (int i = 0; i < 4; i++) {
        loop_send(&a, GET_HELLO, sizeof GET_HELLO - 1);
        assert(loop_wait_responses(&a, i + 1, 2000));
        loop_run(900);
    }
    assert(loop_wait_closed(&b, 2000) && !a.closed);
    assert(cwist_lwip_active_connections(g_srv) == 1);
    loop_close(&b);

    /* teardown aborts what is still open */
    stop();
    loop_run(100);
    assert(a.errored || a.closed);
    loop_close(&a);
}

static void test_listen_defaults_and_failure(void) {
    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    assert(cfg.port == 80 && cfg.max_conns == 4 && cfg.keep_alive);
    assert(cfg.max_header_bytes == 4096 && cfg.max_body_bytes == 16384);
    assert(cfg.idle_timeout_ms == 30000);
    assert(cwist_lwip_listen(NULL, &cfg) == NULL);
    cwist_lwip_server *first = cwist_lwip_listen(g_app, &cfg);
    assert(first);
    assert(cwist_lwip_listen(g_app, &cfg) == NULL); /* port already bound */
    assert(cwist_lwip_active_connections(NULL) == 0);
    cwist_lwip_close(first);
    cwist_lwip_close(NULL);
}

int main(void) {
    for (size_t i = 0; i < sizeof g_big; i++) g_big[i] = (char)('a' + i % 26);
    loop_init();
    g_app = cwist_app_create();
    assert(g_app);
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_app_get(g_app, "/big", big_handler);
    cwist_app_get(g_app, "/close", close_handler);

    test_listen_defaults_and_failure();
    test_keep_alive_and_pipelining();
    test_body_split_and_expect_continue();
    test_framing_errors();
    test_close_large_and_head();
    test_conn_cap_idle_and_teardown();

    cwist_app_destroy(g_app);
    printf("test_lwip_adapter: all passed\n");
    return 0;
}
