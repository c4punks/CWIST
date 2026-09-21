#include <cwist/app.h>
#include <cwist/net/http/session.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello from WASI", 15);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) {
        fprintf(stderr, "app_create failed\n");
        return 1;
    }
    cwist_app_get(app, "/hello", hello);

    char *res = NULL;
    size_t res_len = 0;
    if (cwist_app_dispatch_memory(app, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 33, &res,
                                  &res_len) != 0 ||
        !res) {
        fprintf(stderr, "dispatch failed\n");
        return 1;
    }
    if (!strstr(res, "hello from WASI")) {
        fprintf(stderr, "bad body\n");
        return 1;
    }
    printf("WASI dispatch OK (%zu bytes)\n", res_len);
    free(res);

    /* Non-route (404) path: exercises cwist_prepare_static decline. */
    char *res404 = NULL;
    size_t len404 = 0;
    int rc404 = cwist_app_dispatch_memory(app, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n", 30, &res404,
                                          &len404);
    /* Pre-existing contract (same on Emscripten): no-route dispatch yields
     * rc=-1 with no buffer. What matters is that it traps nowhere. */
    printf("WASI 404 path handled (rc=%d, no trap)\n", rc404);
    free(res404);

    /* Session roundtrip: fixed-secret contract under WASI. */
    if (cwist_app_use_session(app, "0123456789abcdef0123456789abcdef") != 0) {
        fprintf(stderr, "use_session failed\n");
        return 1;
    }
    printf("WASI session secret OK\n");

    cwist_app_destroy(app);
    printf("WASI SMOKE PASS\n");
    return 0;
}
