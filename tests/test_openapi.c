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

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("test_openapi: OK\n");
    return 0;
}
