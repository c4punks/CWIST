#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/core/mem/alloc.h>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *__real_cwist_alloc(size_t size);

static int fail_http_route;
static int route_seen;
static int handler_calls;

void *__wrap_cwist_alloc(size_t size) {
    /* sizeof(cwist_grpc_route) */
    if (fail_http_route && !route_seen && size == 6 * sizeof(void *) + 4 * sizeof(int)) {
        route_seen = 1;
        return __real_cwist_alloc(size);
    }
    if (fail_http_route && route_seen) return NULL;
    return __real_cwist_alloc(size);
}

static void unary_handler(cwist_http_request *req, cwist_http_response *res,
                          const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)message;
    (void)user_ctx;
    handler_calls++;
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, NULL, 0);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    fail_http_route = 1;
    route_seen = 0;
    assert(cwist_app_grpc_unary(app, "cwist.test.Echo", "Call", unary_handler, NULL) == -1);
    fail_http_route = 0;
    assert(route_seen == 1);
    assert(handler_calls == 0);

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    assert(cwist_grpc_encode_message(NULL, 0, 0, &frame, &frame_len) == 0);
    cwist_test_client *client = cwist_test_client_create(app);
    assert(client != NULL);
    cwist_test_client_request_options opts = {
        .body = (const char *)frame,
        .body_len = frame_len,
        .content_type = "application/grpc",
    };
    cwist_http_response *res =
        cwist_test_client_request_ex(client, CWIST_HTTP_POST, "/cwist.test.Echo/Call", &opts);
    assert(res != NULL && res->status_code == CWIST_HTTP_NOT_FOUND);
    cwist_http_response_destroy(res);

    assert(cwist_app_grpc_unary(app, "cwist.test.Echo", "Call", unary_handler, NULL) == 0);
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST, "/cwist.test.Echo/Call", &opts);
    assert(res != NULL && strcmp(cwist_http_header_get(res->headers, "grpc-status"), "0") == 0);
    assert(handler_calls == 1);

    cwist_http_response_destroy(res);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    cwist_free(frame);
    puts("test_grpc_route_alloc: all passed");
    return 0;
}
