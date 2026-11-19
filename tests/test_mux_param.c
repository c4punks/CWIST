#include <cwist/net/http/mux.h>
#include <cwist/net/http/query.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void dummy_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

static void parameterized_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    assert(req->path_params != NULL);
    const char *id = cwist_query_map_get(req->path_params, "id");
    const char *post_id = cwist_query_map_get(req->path_params, "post_id");
    assert(id != NULL && strcmp(id, "123") == 0);
    assert(post_id != NULL && strcmp(post_id, "456") == 0);
}

static void echo_param_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *name = cwist_query_map_get(req->path_params, "name");
    assert(name != NULL);
    cwist_sstring_assign(res->body, name);
}

static void get_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "get");
}

static void head_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "head");
}

static cwist_http_response *serve(cwist_mux_router *router, cwist_http_method_t method,
                                  const char *path) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    assert(req != NULL && res != NULL);
    req->method = method;
    cwist_sstring_assign(req->path, path);
    bool handled = cwist_mux_serve(router, req, res);
    cwist_http_request_destroy(req);
    if (!handled) {
        cwist_http_response_destroy(res);
        return NULL;
    }
    return res;
}

int main(void) {
    printf("Testing Gorilla/Mux style Parametric Router...\n");

    cwist_mux_router *router = cwist_mux_router_create();
    assert(router != NULL);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/users", dummy_handler);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/users/:id/posts/:post_id", parameterized_handler);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/files/:name", echo_param_handler);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/explicit-head", get_handler);
    cwist_mux_handle(router, CWIST_HTTP_HEAD, "/explicit-head", head_handler);

    cwist_http_response *res = serve(router, CWIST_HTTP_GET, "/users");
    assert(res != NULL);
    cwist_http_response_destroy(res);

    res = serve(router, CWIST_HTTP_GET, "/users/123/posts/456");
    assert(res != NULL);
    cwist_http_response_destroy(res);

    assert(serve(router, CWIST_HTTP_GET, "/users/123/posts/456/extra") == NULL);

    res = serve(router, CWIST_HTTP_GET, "/files/John%20Doe%2Fa+b");
    assert(res != NULL && strcmp(res->body->data, "John Doe/a+b") == 0);
    cwist_http_response_destroy(res);

    char long_path[308];
    memcpy(long_path, "/files/", 7);
    memset(long_path + 7, 'A', 300);
    long_path[307] = '\0';
    res = serve(router, CWIST_HTTP_GET, long_path);
    assert(res != NULL && res->body->size == 300);
    cwist_http_response_destroy(res);

    assert(serve(router, CWIST_HTTP_GET, "/files/bad%") == NULL);
    assert(serve(router, CWIST_HTTP_GET, "/files/bad%00") == NULL);
    assert(serve(router, CWIST_HTTP_GET, "/files/bad%1F") == NULL);

    res = serve(router, CWIST_HTTP_HEAD, "/files/a%20b");
    assert(res != NULL && strcmp(res->body->data, "a b") == 0);
    cwist_http_response_destroy(res);

    res = serve(router, CWIST_HTTP_HEAD, "/explicit-head");
    assert(res != NULL && strcmp(res->body->data, "head") == 0);
    cwist_http_response_destroy(res);

    res = serve(router, CWIST_HTTP_POST, "/files/value");
    assert(res != NULL && res->status_code == CWIST_HTTP_METHOD_NOT_ALLOWED);
    const char *allow = cwist_http_header_get(res->headers, "Allow");
    assert(allow != NULL && strcmp(allow, "GET, HEAD") == 0);
    cwist_http_response_destroy(res);

    cwist_mux_router_destroy(router);
    printf("All Mux tests passed!\n");
    return 0;
}
