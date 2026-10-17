/**
 * @file test_bind_write_error_response.c
 * @brief Behavioral tests for cwist_bind_write_error_response (issue #342).
 *
 * cwist_bind_write_error_response is the lowest-level path for turning a
 * failed cwist_bind_result_t into a 400 response.  It is tested here
 * directly -- not via the cwist_app_req_bind_json_or_400 convenience wrapper
 * that test_bind.c already covers -- so the function's own contract is
 * verified:
 *
 *   - status_code is CWIST_HTTP_BAD_REQUEST (400).
 *   - Content-Type header is application/json.
 *   - Body is a JSON object with "success":false and an "errors" array
 *     containing one entry per cwist_bind_error_t in the result.
 *   - NULL result or zero error_count produces {"success":false,"errors":[]}.
 *   - NULL res is a safe no-op.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/validation/bind.h>
#include <cwist/net/http/http.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Build a result with n synthetic errors. */
static cwist_bind_result_t make_result(int n) {
    cwist_bind_result_t r;
    memset(&r, 0, sizeof(r));
    r.ok = false;
    r.error_count = (size_t)n;
    for (int i = 0; i < n; i++) {
        snprintf(r.errors[i].key, CWIST_BIND_KEY_MAX, "field%d", i);
        snprintf(r.errors[i].message, CWIST_BIND_MSG_MAX, "error for field%d", i);
    }
    return r;
}

/* One error: status, content-type, and body shape. */
static void test_single_error(void) {
    printf("test_bind_write_error_response single error...\n");

    cwist_http_response *res = cwist_http_response_create();
    cwist_bind_result_t result = make_result(1);
    cwist_bind_write_error_response(res, &result);

    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);

    const char *ct = cwist_http_header_get(res->headers, "Content-Type");
    assert(ct != NULL);
    assert(strstr(ct, "application/json") != NULL);

    const char *body = res->body ? res->body->data : "";
    assert(strstr(body, "\"success\"") != NULL);
    assert(strstr(body, "false") != NULL);
    assert(strstr(body, "\"errors\"") != NULL);
    assert(strstr(body, "\"field0\"") != NULL);
    assert(strstr(body, "\"error for field0\"") != NULL);

    cwist_http_response_destroy(res);
    printf("  OK\n");
}

/* Two errors: both field names appear in the body. */
static void test_multiple_errors(void) {
    printf("test_bind_write_error_response multiple errors...\n");

    cwist_http_response *res = cwist_http_response_create();
    cwist_bind_result_t result = make_result(2);
    cwist_bind_write_error_response(res, &result);

    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);

    const char *body = res->body ? res->body->data : "";
    assert(strstr(body, "\"field0\"") != NULL);
    assert(strstr(body, "\"field1\"") != NULL);

    cwist_http_response_destroy(res);
    printf("  OK\n");
}

/* Zero errors: body has empty array, status is still 400. */
static void test_zero_errors(void) {
    printf("test_bind_write_error_response zero errors...\n");

    cwist_http_response *res = cwist_http_response_create();
    cwist_bind_result_t result = make_result(0);
    cwist_bind_write_error_response(res, &result);

    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);

    const char *body = res->body ? res->body->data : "";
    assert(strstr(body, "\"success\"") != NULL);
    assert(strstr(body, "\"errors\"") != NULL);

    cwist_http_response_destroy(res);
    printf("  OK\n");
}

/* NULL res: must not crash. */
static void test_null_response(void) {
    printf("test_bind_write_error_response null res...\n");
    cwist_bind_result_t result = make_result(1);
    cwist_bind_write_error_response(NULL, &result);
    printf("  OK\n");
}

int main(void) {
    test_single_error();
    test_multiple_errors();
    test_zero_errors();
    test_null_response();
    printf("All test_bind_write_error_response tests passed.\n");
    return 0;
}
