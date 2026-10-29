/* Bare-metal lwIP smoke (issue #447 Phase 3): the CWIST lwIP adapter and
 * lwIP itself on Cortex-M, client and server pcbs over the loopback netif.
 * Exercises keep-alive, pipelining, a body split across segments, a 413,
 * and a response larger than the send window, then reports the heap/stack
 * high-water marks for the whole stack (CWIST + lwIP). Run with
 * `make baremetal-smoke`. */
#include <cwist/app.h>
#include <cwist/sys/port/lwip.h>

#include "loop_client.h"

#include <stdio.h>
#include <string.h>

extern void board_log(const char *s);
extern size_t board_heap_high_water(void);
extern void board_stack_paint(void);
extern size_t board_stack_used(void);
extern void board_alloc_begin(void);
extern size_t board_alloc_end(void);

static char g_big[8192];
static char g_echo[32];

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello over lwip", 15);
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    int n = snprintf(g_echo, sizeof g_echo, "len=%u", (unsigned)(req->body ? req->body->size : 0));
    cwist_http_response_set_body_ptr(res, g_echo, (size_t)n);
}

static void big_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, g_big, sizeof g_big);
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

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_body_bytes = 1024;
    cwist_lwip_server *srv = cwist_lwip_listen(app, &cfg);
    int fail = check("listen on :80", srv != NULL);
    if (!srv) return fail;

    loop_client c;
    fail |= check("client connects", loop_connect(&c, 80));
    static const char get[] = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    loop_send(&c, get, sizeof get - 1);
    fail |= check("GET /hello", loop_wait_responses(&c, 1, 2000) && strstr(c.rx, "hello over lwip"));
    fail |= check("keep-alive", strstr(c.rx, "Connection: keep-alive") != NULL);

    static const char two[] = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
                              "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nab";
    loop_send(&c, two, sizeof two - 1);
    loop_send(&c, "cdef", 4);
    fail |= check("pipelined + split body", loop_wait_responses(&c, 3, 2000) && strstr(c.rx, "len=6"));

    static const char big[] = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t before = c.rx_len;
    loop_send(&c, big, sizeof big - 1);
    int ok = loop_wait_responses(&c, 4, 5000);
    const char *body = ok ? strstr(c.rx + before, "\r\n\r\n") : NULL;
    fail |= check("8 KB response through a 2-segment window",
                  body && (size_t)(c.rx + c.rx_len - (body + 4)) == sizeof g_big &&
                      memcmp(body + 4, g_big, sizeof g_big) == 0);

    board_alloc_begin();
    loop_send(&c, get, sizeof get - 1);
    int steady_ok = loop_wait_responses(&c, 5, 2000);
    size_t steady_allocs = board_alloc_end();
    fail |= check("zero allocations per steady lwIP response", steady_ok && steady_allocs == 0);
    loop_close(&c);

    loop_client e;
    loop_connect(&e, 80);
    static const char huge[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4096\r\n\r\n";
    loop_send(&e, huge, sizeof huge - 1);
    fail |= check("413 above max_body_bytes",
                  loop_wait_responses(&e, 1, 2000) && strstr(e.rx, "HTTP/1.1 413") &&
                      loop_wait_closed(&e, 2000));
    loop_close(&e);

    loop_run(200);
    fail |= check("all connections released", cwist_lwip_active_connections(srv) == 0);
    cwist_lwip_close(srv);
    cwist_app_destroy(app);

    char buf[96];
    snprintf(buf, sizeof buf, "steady allocs per request: %u\n", (unsigned)steady_allocs);
    board_log(buf);
    snprintf(buf, sizeof buf, "heap high water: %u bytes, stack used: %u bytes\n",
             (unsigned)board_heap_high_water(), (unsigned)board_stack_used());
    board_log(buf);
    board_log(fail ? "baremetal-lwip-smoke: FAIL\n" : "baremetal-lwip-smoke: PASS\n");
    return fail;
}
