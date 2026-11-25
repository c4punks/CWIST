#include <cwist/net/http/http.h>
#include <cwist/sys/app/middleware.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

static bool next_called = false;
static int fail_header = 0;
static int fail_header_preflight = 0;

cwist_error_t __real_cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                           const char *value);
cwist_error_t __wrap_cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                           const char *value) {
    if (fail_header) return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    if (fail_header_preflight && strcmp(key, "Access-Control-Allow-Methods") == 0) {
        return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    }
    return __real_cwist_http_header_add(head, key, value);
}

void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
    next_called = true;
}

void test_cors_options() {
    printf("Testing CORS OPTIONS (Preflight)...\n");
    cwist_middleware_func cors = cwist_mw_cors();

    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_OPTIONS;
    cwist_http_header_add(&req->headers, "Origin", "https://example.com");
    cwist_http_header_add(&req->headers, "Access-Control-Request-Method", "GET");

    cwist_http_response *res = cwist_http_response_create();

    next_called = false;
    cors(req, res, dummy_next);

    // Check short-circuit
    assert(next_called == false);

    // Check Status
    assert(res->status_code == CWIST_HTTP_NO_CONTENT);

    // Check Headers
    assert(strcmp(cwist_http_header_get(res->headers, "Access-Control-Allow-Origin"), "*") == 0);
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Methods") != NULL);
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Headers") != NULL);
    assert(cwist_http_header_get(res->headers, "Access-Control-Max-Age") != NULL);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("Passed CORS OPTIONS.\n");
}

void test_cors_simple() {
    printf("Testing CORS Simple Request (GET)...\n");
    cwist_middleware_func cors = cwist_mw_cors();

    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_GET;

    cwist_http_response *res = cwist_http_response_create();

    next_called = false;
    cors(req, res, dummy_next);

    // Check next called
    assert(next_called == true);

    // Check Headers
    assert(strcmp(cwist_http_header_get(res->headers, "Access-Control-Allow-Origin"), "*") == 0);
    // Should NOT have preflight headers
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Methods") == NULL);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("Passed CORS Simple Request.\n");
}

static void test_plain_options_reaches_application(void) {
    cwist_middleware_func cors = cwist_mw_cors();
    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_OPTIONS;
    cwist_http_response *res = cwist_http_response_create();
    next_called = false;
    cors(req, res, dummy_next);
    assert(next_called);
    assert(res->status_code != CWIST_HTTP_NO_CONTENT);
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Methods") == NULL);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
}

static void test_cors_header_failure(void) {
    cwist_middleware_func cors = cwist_mw_cors();

    /* 1. Failure on initial Access-Control-Allow-Origin */
    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_OPTIONS;
    cwist_http_header_add(&req->headers, "Origin", "https://example.com");
    cwist_http_header_add(&req->headers, "Access-Control-Request-Method", "GET");

    cwist_http_response *res = cwist_http_response_create();
    next_called = false;
    fail_header = 1;
    cors(req, res, dummy_next);
    fail_header = 0;
    assert(next_called == false);
    assert(res->status_code == CWIST_HTTP_INTERNAL_ERROR);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);

    /* 2. Failure during preflight header construction */
    req = cwist_http_request_create();
    req->method = CWIST_HTTP_OPTIONS;
    cwist_http_header_add(&req->headers, "Origin", "https://example.com");
    cwist_http_header_add(&req->headers, "Access-Control-Request-Method", "GET");

    res = cwist_http_response_create();
    next_called = false;
    fail_header_preflight = 1;
    cors(req, res, dummy_next);
    fail_header_preflight = 0;
    assert(next_called == false);
    assert(res->status_code == CWIST_HTTP_INTERNAL_ERROR);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
}

int main() {
    test_cors_options();
    test_cors_simple();
    test_plain_options_reaches_application();
    test_cors_header_failure();
    printf("All CORS tests passed!\n");
    return 0;
}
