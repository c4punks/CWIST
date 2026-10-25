/* Phase 0 spike firmware: build a CWIST app and drive it through
 * cwist_app_dispatch_memory() only.  No sockets, no listen loop. */
#include <cwist/app.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern size_t spike_heap_high_water(void);
extern void spike_log(const char *s);
extern void spike_stack_paint(void);
extern size_t spike_stack_used(void);

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

static int probe(cwist_app *app, const char *req, const char *want) {
    char *res = NULL;
    size_t res_len = 0;
    if (cwist_app_dispatch_memory(app, req, strlen(req), &res, &res_len) != 0 || !res) {
        spike_log("dispatch failed\n");
        return 1;
    }
    int ok = memmem(res, res_len, want, strlen(want)) != NULL && memchr(res, 0, res_len) == NULL;
    if (!ok) { spike_log("--- got:\n"); spike_log(res); spike_log("\n---\n"); }
    spike_log(ok ? "PASS " : "FAIL ");
    spike_log(req);
    free(res);
    return !ok;
}

int main(void) {
    spike_stack_paint();
    cwist_app *app = cwist_app_create();
    if (!app) {
        spike_log("app_create failed\n");
        return 1;
    }
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_get(app, "/json", json_handler);
    int fail = 0;
#ifndef SPIKE_ITERS
#define SPIKE_ITERS 3
#endif
    for (int i = 0; i < SPIKE_ITERS; i++) {
        fail |= probe(app, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", "hello from cortex-m");
        fail |= probe(app, "GET /json HTTP/1.1\r\nHost: x\r\n\r\n", "\"board\":\"stm32\"");
        fail |= probe(app, "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", "404");
    }
    char buf[96];
    snprintf(buf, sizeof buf, "heap high water: %u bytes, stack used: %u bytes\n",
             (unsigned)spike_heap_high_water(), (unsigned)spike_stack_used());
    spike_log(buf);
    spike_log(fail ? "SPIKE: FAIL\n" : "SPIKE: PASS\n");
    return fail;
}
