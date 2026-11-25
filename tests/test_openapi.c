/**
 * @file test_openapi.c
 * @brief Runtime OpenAPI endpoint: the served document parses, carries the
 *        metadata and annotations, and follows the live route table.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/openapi.h>
#include <cwist/sys/app/test_client.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#ifdef CWIST_TEST_WRAP
static int fail_assign;
cwist_error_t __real_cwist_sstring_assign(cwist_sstring *str, const char *data);
cwist_error_t __wrap_cwist_sstring_assign(cwist_sstring *str, const char *data) {
    if (fail_assign && data && strncmp(data, "{\"openapi\"", 10) == 0)
        return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_sstring_assign(str, data);
}
#endif

#define FAIL_IF(cond, msg)                      \
    do {                                        \
        if (cond) {                             \
            fprintf(stderr, "FAIL: %s\n", msg); \
            return 1;                           \
        }                                       \
    } while (0)

static void users_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "[]");
    res->status_code = CWIST_HTTP_OK;
}

static cJSON *fetch_spec(cwist_test_client *client, const char *path) {
    cwist_http_response *res = cwist_test_client_get(client, path);
    if (!res) return NULL;
    cJSON *doc = NULL;
    if (res->status_code == CWIST_HTTP_OK && res->body && res->body->data)
        doc = cJSON_Parse(res->body->data);
    cwist_http_response_destroy(res);
    return doc;
}

static const char *str_at(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

int main(void) {
    cwist_app *app = cwist_app_create();
    FAIL_IF(!app, "app create");

    cwist_openapi_meta_t meta = {
        .title = "Test \"API\"",
        .version = "2.3.4",
        .description = "Unit test spec",
    };
    cwist_app_enable_openapi(app, "/spec.json", &meta);
    CWIST_OPENAPI_GET(app, "/users", users_handler);
    cwist_openapi_annotate(app, "GET", "/users", "List users", "users,public");

    cwist_test_client *client = cwist_test_client_create(app);
    FAIL_IF(!client, "client create");

    cJSON *doc = fetch_spec(client, "/spec.json");
    FAIL_IF(!doc, "spec is served as valid JSON");
    FAIL_IF(!str_at(doc, "openapi") || strcmp(str_at(doc, "openapi"), "3.1.0") != 0,
            "openapi version");

    const cJSON *info = cJSON_GetObjectItemCaseSensitive(doc, "info");
    FAIL_IF(!str_at(info, "title") || strcmp(str_at(info, "title"), "Test \"API\"") != 0,
            "title round-trips through escaping");
    FAIL_IF(!str_at(info, "version") || strcmp(str_at(info, "version"), "2.3.4") != 0,
            "info.version");

    const cJSON *paths = cJSON_GetObjectItemCaseSensitive(doc, "paths");
    const cJSON *get = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(paths, "/users"), "get");
    FAIL_IF(!get, "annotated operation present");
    FAIL_IF(!str_at(get, "summary") || strcmp(str_at(get, "summary"), "List users") != 0,
            "operation summary");
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(get, "tags");
    FAIL_IF(cJSON_GetArraySize(tags) != 2, "two tags");
    FAIL_IF(strcmp(cJSON_GetArrayItem(tags, 1)->valuestring, "public") != 0, "second tag");
    FAIL_IF(cJSON_GetObjectItemCaseSensitive(paths, "/spec.json"), "spec route not listed");
    cJSON_Delete(doc);

    /* Routes added later, unannotated, show up on the next request; a path
     * parameter becomes a templated path with a parameter object. */
    cwist_app_post(app, "/users", users_handler);
    cwist_app_get(app, "/users/:id", users_handler);
    doc = fetch_spec(client, "/spec.json");
    FAIL_IF(!doc, "spec after adding routes");
    paths = cJSON_GetObjectItemCaseSensitive(doc, "paths");
    const cJSON *users = cJSON_GetObjectItemCaseSensitive(paths, "/users");
    FAIL_IF(!cJSON_GetObjectItemCaseSensitive(users, "get") ||
                !cJSON_GetObjectItemCaseSensitive(users, "post"),
            "GET and POST share one path item");
    const cJSON *by_id = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(paths, "/users/{id}"), "get");
    FAIL_IF(!by_id, "templated path");
    const cJSON *param = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(by_id, "parameters"), 0);
    FAIL_IF(!str_at(param, "name") || strcmp(str_at(param, "name"), "id") != 0 ||
                strcmp(str_at(param, "in"), "path") != 0,
            "path parameter");
    cJSON_Delete(doc);

    /* A wildcard route becomes a {wildcard} path parameter, not a literal '*'. */
    cwist_app_get(app, "/static/*", users_handler);
    doc = fetch_spec(client, "/spec.json");
    FAIL_IF(!doc, "spec after adding wildcard route");
    paths = cJSON_GetObjectItemCaseSensitive(doc, "paths");
    FAIL_IF(cJSON_GetObjectItemCaseSensitive(paths, "/static/*"), "literal wildcard path");
    const cJSON *wild = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(paths, "/static/{wildcard}"), "get");
    FAIL_IF(!wild, "wildcard templated path");
    param = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(wild, "parameters"), 0);
    FAIL_IF(!str_at(param, "name") || strcmp(str_at(param, "name"), "wildcard") != 0 ||
                strcmp(str_at(param, "in"), "path") != 0,
            "wildcard path parameter");
    cJSON_Delete(doc);

    /* A second app keeps its own document. */
    cwist_app *other = cwist_app_create();
    FAIL_IF(!other, "second app");
    cwist_app_enable_openapi(other, NULL, NULL);
    cwist_test_client *other_client = cwist_test_client_create(other);
    doc = fetch_spec(other_client, "/openapi.json");
    FAIL_IF(!doc, "default path and metadata");
    FAIL_IF(strcmp(str_at(cJSON_GetObjectItemCaseSensitive(doc, "info"), "title"), "CWIST API") != 0,
            "default title");
    FAIL_IF(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(doc, "paths")) != 0,
            "other app's routes do not leak in");
    cJSON_Delete(doc);
    cwist_test_client_destroy(other_client);
    cwist_app_destroy(other);

#ifdef CWIST_TEST_WRAP
    cwist_http_request *failed_req = cwist_http_request_create();
    failed_req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(failed_req->path, "/spec.json");
    cwist_http_response *failed = cwist_http_response_create();
    fail_assign = 1;
    cwist_app_dispatch(app, failed_req, failed);
    fail_assign = 0;
    FAIL_IF(!failed || failed->status_code != CWIST_HTTP_INTERNAL_ERROR,
            "response allocation failure returns 500");
    cwist_http_response_destroy(failed);
    cwist_http_request_destroy(failed_req);
#endif

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("test_openapi: OK\n");
    return 0;
}
