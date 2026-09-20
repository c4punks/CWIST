/* WASI 0.2 (wasm32-wasip2) smoke test: the socket server runtime actually
 * binds and serves over wasi:sockets. Run via `make wasip2-smoke`, which
 * starts the module under wasmtime with a TCP grant and drives it with curl.
 *
 * The binary listens forever (no signals exist to stop the accept loop), so
 * the Makefile target kills the wasmtime process after the curl probe. */
#include <cwist/app.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef WASIP2_SMOKE_PORT
#define WASIP2_SMOKE_PORT 18099
#endif

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello from WASI 0.2", 19);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) {
        fprintf(stderr, "app_create failed\n");
        return 1;
    }
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
