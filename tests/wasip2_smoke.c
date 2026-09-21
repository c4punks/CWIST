/* WASI 0.2 (wasm32-wasip2) smoke test: the socket server runtime actually
 * binds and serves over wasi:sockets. Run via `make wasip2-smoke`, which
 * starts the module under wasmtime with a TCP grant and drives it with curl.
 *
 * The binary listens forever (no signals exist to stop the accept loop), so
 * the Makefile target kills the wasmtime process after the curl probe.
 *
 * The handler runs a sqlite canary query on every request: a stack overflow
 * in the request path silently corrupts linear memory (wasm has no guard
 * page), and sqlite's parser tables are a sensitive tripwire — the Makefile
 * probes twice so a corrupted second request fails the gate. */
#include <cwist/app.h>
#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef WASIP2_SMOKE_PORT
#define WASIP2_SMOKE_PORT 18099
#endif

static cwist_db *g_canary_db;

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *rows = NULL;
    /* ORDER BY exercises more of sqlite's keyword/parser tables — the region
     * a request-path stack overflow was observed corrupting. */
    int db_ok = g_canary_db &&
        cwist_db_query(g_canary_db, "SELECT 1 AS one ORDER BY one", &rows).error.err_i16 == 0 && rows;
    if (rows) cJSON_Delete(rows);
    if (!db_ok) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_http_response_set_body_ptr(res, "db canary failed", 16);
        return;
    }
    cwist_http_response_set_body_ptr(res, "hello from WASI 0.2", 19);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) {
        fprintf(stderr, "app_create failed\n");
        return 1;
    }
    cwist_db_open(&g_canary_db, ":memory:"); /* failure surfaces via the canary */
    cwist_app_get(app, "/hello", hello_handler);

    /* In-memory dispatch must keep working alongside the socket runtime. */
    char *res = NULL;
    size_t res_len = 0;
    if (cwist_app_dispatch_memory(app, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 33, &res,
                                  &res_len) != 0 ||
        !res || !strstr(res, "hello from WASI 0.2")) {
        fprintf(stderr, "dispatch_memory failed\n");
        return 1;
    }
    free(res);
    printf("wasip2: in-memory dispatch OK\n");
    fflush(stdout);

    printf("wasip2: listening on port %d\n", WASIP2_SMOKE_PORT);
    fflush(stdout);
    return cwist_app_listen(app, WASIP2_SMOKE_PORT);
}
