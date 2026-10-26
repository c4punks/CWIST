/* Bare-metal smoke firmware (issue #447): a CWIST app on Cortex-M driven only
 * through cwist_app_dispatch_memory(). Checks routing, JSON bodies, the 404
 * path, the entropy hook, and the Date header contract (present with an RTC,
 * absent without one), then reports heap/stack high-water marks. Run with
 * `make baremetal-smoke`. */
#include <cwist/app.h>
#include <cwist/sys/app/logger.h>
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HELLO_ITERS
#define HELLO_ITERS 3
#endif

extern void board_log(const char *s);
extern size_t board_heap_high_water(void);
extern void board_stack_paint(void);
extern size_t board_stack_used(void);
extern unsigned board_entropy_calls;

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello from cortex-m", 19);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

static void free_body(const void *ptr, size_t len, void *ctx) {
    (void)len;
    (void)ctx;
    free((void *)ptr);
}

static void json_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "board", "stm32");
    cJSON_AddNumberToObject(o, "uptime_ms", 1234);
    char *s = cJSON_PrintUnformatted(o);
    cwist_http_response_set_body_ptr_managed(res, s, strlen(s), free_body, NULL);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cJSON_Delete(o);
}

static int check(const char *what, int ok) {
    board_log(ok ? "PASS " : "FAIL ");
    board_log(what);
    board_log("\n");
    return !ok;
}

static int probe(cwist_app *app, const char *path, const char *want) {
    char req[96];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    char *res = NULL;
    size_t res_len = 0;
    if (cwist_app_dispatch_memory(app, req, strlen(req), &res, &res_len) != 0 || !res)
        return check(path, 0);
    int fail = 0;
    char what[64];
    snprintf(what, sizeof what, "%s body", path);
    fail |= check(what, memmem(res, res_len, want, strlen(want)) != NULL);
    /* NUL bytes once leaked through an unset Date cache (no RTC). */
    snprintf(what, sizeof what, "%s has no NUL bytes", path);
    fail |= check(what, memchr(res, 0, res_len) == NULL);
#ifdef BOARD_RTC
    snprintf(what, sizeof what, "%s carries Date (RTC)", path);
    fail |= check(what, memmem(res, res_len, "\r\nDate: ", 8) != NULL);
#else
    snprintf(what, sizeof what, "%s omits Date (no RTC)", path);
    fail |= check(what, memmem(res, res_len, "\r\nDate:", 7) == NULL);
#endif
    free(res);
    return fail;
}

int main(void) {
    board_stack_paint();
    cwist_app *app = cwist_app_create();
    if (!app) return check("cwist_app_create", 0);
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_get(app, "/json", json_handler);

    int fail = 0;
    for (int i = 0; i < HELLO_ITERS; i++) {
        fail |= probe(app, "/hello", "hello from cortex-m");
        /* the query string makes the parser build a seeded query map */
        fail |= probe(app, "/json?verbose=1", "\"board\":\"stm32\"");
        fail |= probe(app, "/missing", " 404 ");
    }
    fail |= check("query-map hash seed drawn from cwist_port_entropy", board_entropy_calls > 0);

    /* No threads: async logging must decline and lines go out synchronously
     * through newlib's _write (semihosting here, a UART on a board). */
    fail |= check("async logging declines without threads", !cwist_log_async_start(4096));
    cwist_logger *log = cwist_logger_create("fw");
    cwist_logger_set_colors(log, false);
    cwist_logger_info(log, "served %d requests", HELLO_ITERS * 3);
    cwist_logger_destroy(log);
    fflush(NULL);
    cwist_app_destroy(app);

    char buf[96];
    snprintf(buf, sizeof buf, "heap high water: %u bytes, stack used: %u bytes\n",
             (unsigned)board_heap_high_water(), (unsigned)board_stack_used());
    board_log(buf);
    board_log(fail ? "baremetal-smoke: FAIL\n" : "baremetal-smoke: PASS\n");
    return fail;
}
