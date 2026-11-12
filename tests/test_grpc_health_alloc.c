#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/core/mem/alloc.h>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *__real_cwist_alloc(size_t size);

static int fail_second_route;
static int route_allocations;
static int fail_service_copy;

void *__wrap_cwist_alloc(size_t size) {
    if (size == 6 * sizeof(void *) + 4 * sizeof(int)) { /* cwist_grpc_route */
        route_allocations++;
        if (fail_second_route && route_allocations == 2) return NULL;
    }
    if (fail_service_copy && size == 11) return NULL;
    return __real_cwist_alloc(size);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    fail_second_route = 1;
    route_allocations = 0;
    assert(cwist_app_grpc_health(app) == -1);
    fail_second_route = 0;
    assert(route_allocations >= 2);
    assert(cwist_app_grpc_health_set_status(app, "cwist.test", 1) == -1);
    assert(cwist_app_grpc_health_watchers(app) == -1);

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
    cwist_http_response *res = cwist_test_client_request_ex(
        client, CWIST_HTTP_POST, "/grpc.health.v1.Health/Check", &opts);
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "12") == 0);

    cwist_http_response_destroy(res);

    assert(cwist_app_grpc_health(app) == 0);
    static const uint8_t service_request[] = {
        0x0a, 0x0a, 'c', 'w', 'i', 's', 't', '.', 't', 'e', 's', 't'
    };
    cwist_free(frame);
    frame = NULL;
    frame_len = 0;
    assert(cwist_grpc_encode_message(service_request, sizeof(service_request), 0,
                                     &frame, &frame_len) == 0);
    opts.body = (const char *)frame;
    opts.body_len = frame_len;
    fail_service_copy = 1;
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/grpc.health.v1.Health/Check", &opts);
    fail_service_copy = 0;
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "13") == 0);
    cwist_http_response_destroy(res);

    fail_service_copy = 1;
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/grpc.health.v1.Health/Watch", &opts);
    fail_service_copy = 0;
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "13") == 0);
    assert(res->body == NULL || res->body->size == 0);

    cwist_http_response_destroy(res);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    cwist_free(frame);
    puts("test_grpc_health_alloc: all passed");
    return 0;
}
