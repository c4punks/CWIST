/**
 * @file test_flash.c
 * @brief Unit tests for flash messages, zero-guards, and json serialization.
 */

#include <cwist/sys/session/flash.h>
#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_flash_null_and_zero_guards(void) {
    printf("test_flash_null_and_zero_guards...\n");

    // NULL request or NULL keys
    cwist_flash_set(NULL, "key", "val");
    assert(cwist_flash_peek(NULL, "key") == NULL);
    assert(cwist_flash_get(NULL, "key") == NULL);
    assert(cwist_flash_pop_all_json(NULL) == NULL);

    cwist_http_request req;
    memset(&req, 0, sizeof(req));

    // Empty / NULL flash
    assert(cwist_flash_peek(&req, "key") == NULL);
    assert(cwist_flash_get(&req, "key") == NULL);
    assert(cwist_flash_pop_all_json(&req) == NULL);

    // Dummy map with size 0: should not divide by zero
    cwist_query_map map;
    memset(&map, 0, sizeof(map));
    map.size = 0;
    map.buckets = NULL;
    req.flash = &map;

    assert(cwist_flash_get(&req, "key") == NULL);
    assert(cwist_flash_pop_all_json(&req) == NULL);
    req.flash = NULL;
}

static void test_flash_basic_and_json(void) {
    printf("test_flash_basic_and_json...\n");

    cwist_http_request req;
    memset(&req, 0, sizeof(req));

    cwist_flash_set(&req, "info", "File uploaded \"successfully\"\\test");
    cwist_flash_set(&req, "status", "active");

    // Peek does not remove
    assert(cwist_flash_peek(&req, "info") != NULL);
    assert(cwist_flash_peek(&req, "info") != NULL);

    // Pop all as JSON
    char *json = cwist_flash_pop_all_json(&req);
    assert(json != NULL);

    cJSON *parsed = cJSON_Parse(json);
    assert(parsed != NULL);
    assert(cJSON_GetObjectItem(parsed, "status") != NULL);
    assert(strcmp(cJSON_GetObjectItem(parsed, "status")->valuestring, "active") == 0);
    assert(cJSON_GetObjectItem(parsed, "info") != NULL);
    cJSON_Delete(parsed);
    cwist_free(json);

    // Flash entries should now be cleared
    assert(cwist_flash_peek(&req, "info") == NULL);
    assert(req.flash->count == 0);
    assert(cwist_flash_pop_all_json(&req) == NULL);

    // Test cwist_flash_get pop
    cwist_flash_set(&req, "msg", "hello");
    const char *val = cwist_flash_get(&req, "msg");
    assert(val != NULL);
    assert(strcmp(val, "hello") == 0);
    cwist_free((void *)val);
    assert(req.flash->count == 0);
    assert(cwist_flash_get(&req, "msg") == NULL);

    cwist_query_map_destroy(req.flash);
}

static void test_flash_json_control_characters(void) {
    cwist_http_request req;
    memset(&req, 0, sizeof(req));
    cwist_flash_set(&req, "line\nkey", "first\nsecond\t\x01");
    char *json = cwist_flash_pop_all_json(&req);
    assert(json != NULL);
    assert(strstr(json, "\\n") != NULL);
    assert(strstr(json, "\\t") != NULL);
    assert(strstr(json, "\\u0001") != NULL);
    cJSON *parsed = cJSON_Parse(json);
    assert(parsed != NULL);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(parsed, "line\nkey");
    assert(cJSON_IsString(value));
    assert(strcmp(value->valuestring, "first\nsecond\t\x01") == 0);
    assert(req.flash->count == 0);
    cJSON_Delete(parsed);
    cwist_free(json);
    cwist_query_map_destroy(req.flash);
}

static void session_cookie_value(cwist_http_response *res, char *out, size_t cap) {
    const char *cookie = cwist_http_header_get(res->headers, "Set-Cookie");
    assert(cookie != NULL);
    const char *start = strchr(cookie, '=');
    const char *end = strchr(cookie, ';');
    assert(start != NULL);
    start++;
    if (!end) end = cookie + strlen(cookie);
    size_t len = (size_t)(end - start);
    assert(len < cap);
    memcpy(out, start, len);
    out[len] = '\0';
}

static void test_flash_survives_redirect(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "flash-test-secret") == 0);
    cwist_http_request *first = cwist_http_request_create();
    cwist_http_response *first_res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, first, first_res);
    assert(session != NULL);
    cwist_flash_set(first, "notice", "saved");
    assert(cwist_session_get(session, "_cwist_flash") != NULL);
    assert(cwist_session_commit(session, first_res) == 0);
    char cookie[4096];
    session_cookie_value(first_res, cookie, sizeof(cookie));
    char cookie_header[8192];
    snprintf(cookie_header, sizeof(cookie_header), "cwist_session=%s", cookie);

    cwist_http_request *second = cwist_http_request_create();
    cwist_http_response *second_res = cwist_http_response_create();
    cwist_http_header_add(&second->headers, "Cookie", cookie_header);
    cwist_session_t *next_session = cwist_session_start(app, second, second_res);
    assert(next_session != NULL);
    const char *notice = cwist_flash_get(second, "notice");
    assert(notice != NULL && strcmp(notice, "saved") == 0);
    cwist_free((void *)notice);
    assert(cwist_session_get(next_session, "_cwist_flash") == NULL);
    assert(cwist_flash_get(second, "notice") == NULL);
    assert(cwist_session_commit(next_session, second_res) == 0);
    session_cookie_value(second_res, cookie, sizeof(cookie));
    snprintf(cookie_header, sizeof(cookie_header), "cwist_session=%s", cookie);

    cwist_http_request *third = cwist_http_request_create();
    cwist_http_response *third_res = cwist_http_response_create();
    cwist_http_header_add(&third->headers, "Cookie", cookie_header);
    assert(cwist_session_start(app, third, third_res) != NULL);
    assert(cwist_flash_get(third, "notice") == NULL);

    cwist_http_request_destroy(first);
    cwist_http_response_destroy(first_res);
    cwist_http_request_destroy(second);
    cwist_http_response_destroy(second_res);
    cwist_http_request_destroy(third);
    cwist_http_response_destroy(third_res);
    cwist_app_destroy(app);
}

int main(void) {
    test_flash_survives_redirect();
    test_flash_null_and_zero_guards();
    test_flash_basic_and_json();
    test_flash_json_control_characters();
    printf("All test_flash tests passed!\n");
    return 0;
}
