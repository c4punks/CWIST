/**
 * @file test_regression_batch.c
 * @brief Regression checks for a batch of small, independent bugs: route URL
 *        building, multipart parameters, cookie prefixes, request binding,
 *        header whitespace, template escaping and number formatting, JWT
 *        segment encoding, healed DB inserts, and per-route middleware order.
 *        Second batch: WAF body policy, test client multipart length and
 *        cookie deletion, and locale-independent HTTP dates.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/core/validation/bind.h>
#include <cwist/core/template/template.h>
#include <cwist/net/http/cookie.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/multipart.h>
#include <cwist/net/http/mux.h>
#include <cwist/net/http/query.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/sys/app/waf.h>
#ifdef CWIST_JWT
#include <cwist/security/jwt/jwt.h>
#endif
#ifdef CWIST_DB
#include <cwist/core/db/sql.h>
#endif
#include <cjson/cJSON.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <time.h>

static void noop_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

static void test_url_for(void) {
    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_get_named(app, "/users/:id/posts/", "user_posts", noop_handler);
    cwist_query_map *params = cwist_query_map_create();
    cwist_query_map_set(params, "id", "1/../admin?x=1");
    char *url = cwist_url_for(app, "user_posts", params);
    assert(url);
    assert(strcmp(url, "/users/1%2F..%2Fadmin%3Fx%3D1/posts/") == 0);
    free(url);
    cwist_query_map_set(params, "id", "42");
    url = cwist_url_for(app, "user_posts", params);
    assert(url && strcmp(url, "/users/42/posts/") == 0);
    free(url);
    cwist_query_map_destroy(params);
    cwist_app_destroy(app);
    printf("  url_for OK\n");
}

static void test_multipart_quoted_param(void) {
    const char *body = "--B\r\nContent-Disposition: form-data; filename=\"x; name=admin; y\"; "
                       "name=\"real\"\r\n\r\nv\r\n--B--\r\n";
    cwist_multipart_result *res = cwist_multipart_parse(body, strlen(body), "B");
    assert(res && res->fields);
    assert(strcmp(res->fields->name, "real") == 0);
    assert(strcmp(res->fields->filename, "x; name=admin; y") == 0);
    cwist_multipart_result_destroy(res);

    char *b = cwist_multipart_extract_boundary(
        "multipart/form-data; foo=\"a; boundary=evil\"; boundary=real");
    assert(b && strcmp(b, "real") == 0);
    free(b);
    printf("  multipart OK\n");
}

static void test_cookie_prefix_without_options(void) {
    cwist_http_response *res = cwist_http_response_create();
    assert(cwist_cookie_set(res, "__Host-sid", "x", NULL) == -1);
    assert(cwist_cookie_set(res, "__Secure-sid", "x", NULL) == -1);
    assert(cwist_cookie_set(res, "sid", "x", NULL) == 0);
    cwist_http_response_destroy(res);
    printf("  cookie prefixes OK\n");
}

typedef struct {
    double price;
    uint64_t count;
    long double ratio;
    double big;
} bind_target_t;

static void test_bind_numbers(void) {
    static const cwist_bind_field_t fields[] = {
        CWIST_BIND_FIELD(bind_target_t, price, "price", NULL),
        CWIST_BIND_FIELD(bind_target_t, count, "count", NULL),
        CWIST_BIND_FIELD(bind_target_t, ratio, "ratio", NULL),
        CWIST_BIND_FIELD(bind_target_t, big, "big", NULL),
    };
    cwist_bind_schema_t schema = CWIST_BIND_SCHEMA(bind_target_t, fields);
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body,
                         "{\"price\":1234567.89,\"count\":7,\"ratio\":2.5,\"big\":1e20}");
    bind_target_t out = {0};
    cwist_bind_result_t result;
    assert(cwist_app_req_bind_json(req, &schema, &out, &result));
    assert(out.price == 1234567.89);
    assert(out.count == 7);
    assert(out.ratio == 2.5L);
    assert(out.big == 1e20);

    cwist_sstring_assign(req->body, "{\"count\":\" -5\"}");
    assert(!cwist_app_req_bind_json(req, &schema, &out, &result));
    cwist_http_request_destroy(req);
    printf("  bind numbers OK\n");
}

static void test_header_trailing_ows(void) {
    cwist_http_request *req = cwist_http_parse_request(
        "GET / HTTP/1.1\r\nHost: a\r\nX-CSRF-Token: abc \t\r\nOrigin: https://a.com \r\n\r\n");
    assert(req);
    assert(strcmp(cwist_http_header_get(req->headers, "X-CSRF-Token"), "abc") == 0);
    assert(strcmp(cwist_http_header_get(req->headers, "Origin"), "https://a.com") == 0);
    cwist_http_request_destroy(req);
    printf("  header OWS OK\n");
}

static void test_template(void) {
    cJSON *ctx = cJSON_Parse("{\"x\":\"a<b\",\"n\":20261120,\"p\":1234567.89,\"f\":0.1}");
    cwist_sstring *out =
        cwist_template_render("{{ x | escape | upper }}|{{ n }}|{{ p }}|{{ f }}", ctx);
    assert(out);
    assert(strcmp(out->data, "A&LT;B|20261120|1234567.89|0.1") == 0);
    cwist_sstring_destroy(out);
    cJSON_Delete(ctx);
    printf("  template OK\n");
}

#ifdef CWIST_JWT
static void test_jwt_canonical_segments(void) {
    char *tok = cwist_jwt_sign("{\"sub\":\"1\"}", "secret", 3600);
    assert(tok);
    cwist_jwt_claims *claims = cwist_jwt_verify(tok, "secret");
    assert(claims);
    cwist_jwt_claims_destroy(claims);

    size_t n = strlen(tok);
    char *padded = malloc(n + 3);
    memcpy(padded, tok, n);
    memcpy(padded + n, "==", 3);
    assert(cwist_jwt_verify(padded, "secret") == NULL);
    free(padded);

    /* 32-byte signature = 43 chars; the last one carries 2 unused bits. */
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char *flipped = strdup(tok);
    char *last = flipped + n - 1;
    *last = alpha[(strchr(alpha, *last) - alpha) ^ 1];
    assert(cwist_jwt_verify(flipped, "secret") == NULL);
    free(flipped);
    free(tok);
    printf("  jwt OK\n");
}
#endif

#ifdef CWIST_DB
static void test_db_insert_healed_numbers(void) {
    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    assert(err.error.err_i16 == 0 && db);
    cwist_db_exec(db, "CREATE TABLE t(a, b)");
    err = cwist_db_insert_healed(db, "t", "{\"a\": 20261120, \"b\": 1234567.89}", NULL, NULL);
    assert(err.error.err_i16 == 0);
    cJSON *rows = NULL;
    cwist_db_query(db, "SELECT a = 20261120 AS a_ok, b = 1234567.89 AS b_ok FROM t", &rows);
    char *text = cJSON_PrintUnformatted(rows);
    assert(text && strstr(text, "\"a_ok\":\"1\"") && strstr(text, "\"b_ok\":\"1\""));
    free(text);
    cJSON_Delete(rows);
    cwist_db_close(db);
    printf("  db insert_healed OK\n");
}
#endif

static char order_log[8];
static size_t order_len;

static void mw_first(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    order_log[order_len++] = '1';
    next(req, res);
}

static void mw_second(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    order_log[order_len++] = '2';
    next(req, res);
}

static void order_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
    order_log[order_len++] = 'h';
}

static void test_route_middleware_order(void) {
    cwist_mux_router *mux = cwist_mux_router_create();
    cwist_mux_handle(mux, CWIST_HTTP_GET, "/x", order_handler);
    cwist_mux_route *route = cwist_mux_find_route(mux, CWIST_HTTP_GET, "/x");
    assert(route);
    cwist_mux_route_use(route, mw_first);
    cwist_mux_route_use(route, mw_second);
    cwist_http_request *req = cwist_http_parse_request("GET /x HTTP/1.1\r\nHost: a\r\n\r\n");
    cwist_http_response *res = cwist_http_response_create();
    assert(cwist_mux_serve(mux, req, res));
    assert(order_len == 3 && memcmp(order_log, "12h", 3) == 0);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_mux_router_destroy(mux);
    printf("  route middleware order OK\n");
}

static size_t last_body_len;

static void body_len_handler(cwist_http_request *req, cwist_http_response *res) {
    last_body_len = req->body ? req->body->size : 0;
    res->status_code = CWIST_HTTP_OK;
}

static void test_waf_body_policy(void) {
    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_use(app, cwist_mw_waf_lite());
    cwist_app_post(app, "/up", body_len_handler);
    cwist_test_client *client = cwist_test_client_create(app);
    assert(client);

    /* A PNG header holds NUL and control bytes: binary uploads pass. */
    static const char png[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\x01\x02";
    cwist_http_response *res = cwist_test_client_post_multipart(
        client, "/up", "file", "a.png", "image/png", png, sizeof(png) - 1);
    assert(res && res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);

    cwist_test_client_request_options opts = {0};
    opts.body = png;
    opts.body_len = sizeof(png) - 1;
    opts.content_type = "application/octet-stream";
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST, "/up", &opts);
    assert(res && res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);

    /* Text bodies, multipart file names and text fields are still screened. */
    res = cwist_test_client_post_json(client, "/up", "{\"q\":\"1 union select 2\"}");
    assert(res && res->status_code == CWIST_HTTP_BAD_REQUEST);
    cwist_http_response_destroy(res);
    res = cwist_test_client_post_multipart(client, "/up", "file", "<script>.png", "image/png",
                                           png, sizeof(png) - 1);
    assert(res && res->status_code == CWIST_HTTP_BAD_REQUEST);
    cwist_http_response_destroy(res);
    res = cwist_test_client_post_multipart(client, "/up", "note", "x.txt", "text/plain",
                                           "fine", 4);
    assert(res && res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);
    opts.body = "--B\r\nContent-Disposition: form-data; name=\"q\"\r\n\r\n"
                "<script>alert(1)</script>\r\n--B--\r\n";
    opts.body_len = 0;
    opts.content_type = "multipart/form-data; boundary=B";
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST, "/up", &opts);
    assert(res && res->status_code == CWIST_HTTP_BAD_REQUEST);
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("  waf body policy OK\n");
}

static void test_client_multipart_length(void) {
    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_post(app, "/up", body_len_handler);
    cwist_test_client *client = cwist_test_client_create(app);
    cwist_http_response *res =
        cwist_test_client_post_multipart(client, "/up", "f", "a.txt", "text/plain", "hi", 2);
    assert(res && res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);
    const char *boundary = "CWISTTestClientBoundary7MA4YWxkTrZu0gW";
    size_t expected = strlen("--") + strlen(boundary) + 2 +
                      strlen("Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n") +
                      strlen("Content-Type: text/plain\r\n\r\n") + 2 + strlen("\r\n--") +
                      strlen(boundary) + strlen("--\r\n");
    assert(last_body_len == expected);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("  test client multipart length OK\n");
}

static void set_cookie_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *mode = req->path ? strrchr(req->path->data, '/') : NULL;
    if (mode && strcmp(mode, "/login") == 0)
        cwist_http_header_add(&res->headers, "Set-Cookie", "sid=abc; Path=/");
    else if (mode && strcmp(mode, "/logout") == 0)
        cwist_http_header_add(&res->headers, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    else if (mode && strcmp(mode, "/expire") == 0)
        cwist_http_header_add(&res->headers, "Set-Cookie",
                              "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    else if (mode && strcmp(mode, "/keep") == 0)
        cwist_http_header_add(&res->headers, "Set-Cookie",
                              "sid=new; Max-Age=60; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    res->status_code = CWIST_HTTP_OK;
}

static void hit(cwist_test_client *client, const char *path) {
    cwist_http_response *res = cwist_test_client_get(client, path);
    assert(res && res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);
}

static void test_client_cookie_deletion(void) {
    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_get(app, "/c/login", set_cookie_handler);
    cwist_app_get(app, "/c/logout", set_cookie_handler);
    cwist_app_get(app, "/c/expire", set_cookie_handler);
    cwist_app_get(app, "/c/keep", set_cookie_handler);
    cwist_test_client *client = cwist_test_client_create(app);
    hit(client, "/c/login");
    assert(strcmp(cwist_test_client_get_cookie(client, "sid"), "abc") == 0);
    hit(client, "/c/logout");
    assert(cwist_test_client_get_cookie(client, "sid") == NULL);
    hit(client, "/c/login");
    hit(client, "/c/expire");
    assert(cwist_test_client_get_cookie(client, "sid") == NULL);
    /* Max-Age wins over a past Expires. */
    hit(client, "/c/keep");
    assert(strcmp(cwist_test_client_get_cookie(client, "sid"), "new") == 0);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("  test client cookie deletion OK\n");
}

static void test_http_date_locale(void) {
    /* Use a non-English locale when the system has one; the result must not
     * depend on it either way. */
    const char *names[] = {"ko_KR.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (setlocale(LC_ALL, names[i])) break;
    char buf[64];
    cwist_http_format_date((time_t)784111777, buf, sizeof(buf));
    assert(strcmp(buf, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
    assert(cwist_http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT") == (time_t)784111777);
    assert(cwist_http_parse_date("Sunday, 06-Nov-94 08:49:37 GMT") == (time_t)784111777);
    assert(cwist_http_parse_date("Sun Nov  6 08:49:37 1994") == (time_t)784111777);
    assert(cwist_http_parse_date("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    assert(cwist_http_parse_date("Sun, 06 Nov 1994 08:49:37 PST") == (time_t)-1);
    assert(cwist_http_parse_date("Sun, 06 Foo 1994 08:49:37 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT junk") == (time_t)-1);
    setlocale(LC_ALL, "C");
    printf("  http date locale OK\n");
}

int main(void) {
    printf("test_regression_batch...\n");
    test_waf_body_policy();
    test_client_multipart_length();
    test_client_cookie_deletion();
    test_http_date_locale();
    test_url_for();
    test_multipart_quoted_param();
    test_cookie_prefix_without_options();
    test_bind_numbers();
    test_header_trailing_ows();
    test_template();
#ifdef CWIST_JWT
    test_jwt_canonical_segments();
#endif
#ifdef CWIST_DB
    test_db_insert_healed_numbers();
#endif
    test_route_middleware_order();
    printf("All regression batch tests passed.\n");
    return 0;
}
