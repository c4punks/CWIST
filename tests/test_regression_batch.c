/**
 * @file test_regression_batch.c
 * @brief Regression checks for a batch of small, independent bugs: route URL
 *        building, multipart parameters, cookie prefixes, request binding,
 *        header whitespace, number formatting, JWT
 *        segment encoding, healed DB inserts, and per-route middleware order.
 *        Second batch: WAF body policy, test client multipart length and
 *        cookie deletion, and locale-independent HTTP dates.
 *        Third batch: impossible HTTP dates, tagless HTML elements, sstring
 *        appends after embedded NULs, CSS declaration checks, Prometheus
 *        metric types, and per-statement DB settings.
 *        Fourth batch: JSON heal renames, Redis argument errors, CSRF keys
 *        across workers, htmx boosted requests, NukeDB deserialize, raw text
 *        elements, test client cookie paths, and multiport sub-app settings.
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
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/metrics/metrics.h>
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/template/template.h>
#include <cwist/net/http/html_response.h>
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/sys/app/csrf.h>
#include <cwist/net/http/session.h>
#include <cwist/sys/sys_info.h>
#ifdef CWIST_DB
#include <cwist/core/db/nuke_db.h>
#include <sqlite3.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
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

/* #966: impossible calendar dates are rejected instead of normalized. */
static void test_http_date_calendar(void) {
    assert(cwist_http_parse_date("Tue, 31 Feb 2026 00:00:00 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Sun, 29 Feb 2026 00:00:00 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Thu, 31 Apr 2026 00:00:00 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Sat, 29 Feb 1900 00:00:00 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Thursday, 31-Jun-27 00:00:00 GMT") == (time_t)-1);
    assert(cwist_http_parse_date("Mon Feb 30 00:00:00 2026") == (time_t)-1);
    assert(cwist_http_parse_date("Thu, 29 Feb 2024 00:00:00 GMT") == (time_t)1709164800);
    assert(cwist_http_parse_date("Tue, 29 Feb 2000 00:00:00 GMT") == (time_t)951782400);
    assert(cwist_http_parse_date("Thu, 31 Dec 2026 23:59:59 GMT") == (time_t)1798761599);
    printf("  http date calendar OK\n");
}

/* #964: a tagless element renders its text and then its children. */
static void test_html_fragment_children(void) {
    cwist_html_element_t *frag = cwist_html_element_create("");
    assert(frag != NULL);
    cwist_html_element_set_text(frag, "a&b");
    cwist_html_element_t *p = cwist_html_element_create("p");
    cwist_html_element_set_text(p, "hi");
    cwist_html_element_add_child(frag, p);
    cwist_html_element_t *em = cwist_html_element_create("em");
    cwist_html_element_set_text(em, "x");
    cwist_html_element_add_child(frag, em);
    cwist_sstring *out = cwist_html_render(frag);
    assert(out && strcmp(out->data, "a&amp;b<p>hi</p><em>x</em>") == 0);
    cwist_sstring_destroy(out);
    cwist_html_element_destroy(frag);
    printf("  html fragment children OK\n");
}

/* #963: append and append_escaped keep bytes after an embedded NUL. */
static void test_sstring_append_after_nul(void) {
    cwist_sstring *s = cwist_sstring_create();
    assert(s != NULL);
    cwist_sstring_assign_len(s, "ab\0cd", 5);
    cwist_sstring_append(s, "X");
    assert(s->size == 6 && memcmp(s->data, "ab\0cdX", 7) == 0);
    cwist_sstring_append_escaped(s, "<");
    assert(s->size == 10 && memcmp(s->data, "ab\0cdX&lt;", 11) == 0);
    cwist_sstring_rtrim(s);
    assert(s->size == 10);
    cwist_error_t err = cwist_sstring_change_size(s, 4, false);
    cwist_error_dispose(&err);
    assert(s->size == 10);
    cwist_sstring_destroy(s);
    printf("  sstring append after NUL OK\n");
}

/* #970: a declaration cannot leave a comment, string or escape open. */
static void test_css_declaration_checks(void) {
    cwist_css_scope scope;
    cwist_css_scope_init(&scope, "t");
    assert(cwist_css_scope_add_rule(&scope, "card", "color: red /*") == -1);
    assert(cwist_css_scope_add_rule(&scope, "card", "color: red /* x * /") == -1);
    assert(cwist_css_scope_add_rule(&scope, "card", "content: \"open") == -1);
    assert(cwist_css_scope_add_rule(&scope, "card", "content: 'a\nb'") == -1);
    assert(cwist_css_scope_add_rule(&scope, "card", "color: red\\") == -1);
    assert(cwist_css_scope_add_rule(&scope, "card", "color: red; /* note */ margin: 0") == 0);
    assert(cwist_css_scope_add_rule(&scope, "card", "content: \"a/*b\"; font: 'x\\'y'") == 0);
    cwist_css_scope_destroy(&scope);
    printf("  css declaration checks OK\n");
}

/* #971: pending SQEs are a gauge, each _count sample is declared, and
 * values above 2^32 are not truncated. */
static void test_metrics_exposition(void) {
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    assert(reg != NULL);
    cwist_metrics_reset(reg);
    cwist_metric_add(reg, CWIST_METRIC_REQUESTS_TOTAL, (uintmax_t)5000000000ULL);
    char *text = cwist_metrics_render_prometheus(reg);
    assert(text != NULL);
    assert(strstr(text, "# TYPE cwist_io_uring_sqes_pending gauge\n"));
    assert(strstr(text, "\ncwist_requests_total 5000000000.000\n"));
    assert(strstr(text, "# TYPE cwist_requests_total_count counter\n"
                        "cwist_requests_total_count 1\n"));
    /* Every sample line belongs to a family declared just before it. */
    for (const char *line = text; *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len && line[0] != '#') {
            const char *sp = memchr(line, ' ', len);
            assert(sp != NULL);
            /* Built with memcpy: GCC 13 -Wformat-truncation reports a
             * %.*s argument here as possibly null under -Werror. */
            char decl[160];
            size_t name_len = (size_t)(sp - line);
            assert(name_len + 9 < sizeof(decl));
            memcpy(decl, "# TYPE ", 7);
            memcpy(decl + 7, line, name_len);
            decl[7 + name_len] = ' ';
            decl[8 + name_len] = '\0';
            assert(strstr(text, decl) && strstr(text, decl) < line);
        }
        line += len + (nl ? 1 : 0);
    }
    free(text);
    cwist_metrics_reset(reg);
    printf("  metrics exposition OK\n");
}

#ifdef CWIST_DB
/* #962: statements do not change connection settings. */
static int pragma_int(cwist_db *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void test_db_statement_settings(void) {
    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    assert(cwist_error_is_ok(&err) && db);
    cwist_error_dispose(&err);
    int temp_store = pragma_int(db, "PRAGMA temp_store;");
    int cache_spill = pragma_int(db, "PRAGMA cache_spill;");
    err = cwist_db_exec(db, "CREATE TABLE a(x); CREATE TABLE b(x); CREATE TABLE c(x);");
    cwist_error_dispose(&err);
    cJSON *rows = NULL;
    err = cwist_db_query(db,
                         "SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.x = c.x "
                         "WHERE a.x = 1 AND b.x = 1 OR c.x = 2",
                         &rows);
    cwist_error_dispose(&err);
    cJSON_Delete(rows);
    assert(pragma_int(db, "PRAGMA temp_store;") == temp_store);
    assert(pragma_int(db, "PRAGMA cache_spill;") == cache_spill);
    cwist_db_close(db);
    printf("  db statement settings OK\n");
}
#endif

/* #980: a fuzzy rename does not read the freed key. */
static void test_json_heal_fuzzy_rename(void) {
    static const cwist_schema_field_t fields[] = {{"user_id", {NULL}, CWIST_FIELD_INT, true}};
    static const cwist_schema_t schema = {fields, 1};
    cJSON *obj = cJSON_Parse("{\"userid\": 7}");
    assert(obj != NULL);
    char log[256] = {0};
    assert(cwist_json_schema_align(obj, &schema, log, sizeof(log)) == 1);
    assert(cJSON_GetObjectItemCaseSensitive(obj, "user_id") != NULL);
    assert(cJSON_GetObjectItemCaseSensitive(obj, "userid") == NULL);
    assert(strstr(log, "'userid'") != NULL);
    cJSON_Delete(obj);
    printf("  json heal fuzzy rename OK\n");
}

/* #982: invalid Redis arguments are errors, and outputs are cleared. */
static void test_redis_invalid_arguments(void) {
    char *out = (char *)0x1;
    cwist_error_t e = cwist_redis_get(NULL, "k", &out);
    assert(!cwist_error_is_ok(&e) && out == NULL);
    e = cwist_redis_expire(NULL, "k", 10);
    assert(!cwist_error_is_ok(&e));
    e = cwist_redis_set(NULL, "k", "v");
    assert(!cwist_error_is_ok(&e));
    e = cwist_redis_pool_setex(NULL, "k", "v", 10);
    assert(!cwist_error_is_ok(&e));
    printf("  redis invalid arguments OK\n");
}

/* #987: a token issued by one worker is accepted by another. */
static void test_csrf_across_workers(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use(app, cwist_mw_csrf(app)); /* no session secret */
    cwist_app_get(app, "/form", noop_handler);
    cwist_app_post(app, "/form", noop_handler);
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        cwist_test_client *c = cwist_test_client_create(app);
        cwist_http_response *r = cwist_test_client_get(c, "/form");
        const char *tok = cwist_test_client_get_cookie(c, "csrf_token");
        ssize_t w = tok ? write(fds[1], tok, strlen(tok)) : 0;
        (void)w;
        _exit(r && tok ? 0 : 1);
    }
    close(fds[1]);
    char token[256] = {0};
    ssize_t n = read(fds[0], token, sizeof(token) - 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && n > 0);
    cwist_test_client *c = cwist_test_client_create(app);
    cwist_test_client_kv hdr = {"X-CSRF-Token", token};
    cwist_test_client_kv cookie = {"csrf_token", token};
    cwist_test_client_request_options opts = {0};
    opts.headers = &hdr;
    opts.header_count = 1;
    opts.cookies = &cookie;
    opts.cookie_count = 1;
    cwist_http_response *r = cwist_test_client_request_ex(c, CWIST_HTTP_POST, "/form", &opts);
    assert(r && r->status_code != CWIST_HTTP_FORBIDDEN);
    cwist_http_response_destroy(r);
    cwist_test_client_destroy(c);
    cwist_app_destroy(app);
    printf("  csrf across workers OK\n");
}

/* #988: htmx boosted navigation asks for the full page. */
static void test_htmx_boosted(void) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "HX-Request", "true");
    assert(cwist_http_request_wants_fragment(req));
    cwist_http_header_add(&req->headers, "HX-Boosted", "true");
    assert(!cwist_http_request_wants_fragment(req));
    assert(strstr(CWIST_HTML_FRAGMENT_VARY, "HX-Boosted") != NULL);
    cwist_http_request_destroy(req);
    printf("  htmx boosted OK\n");
}

/* #994: script and style text is raw, and cannot close the element. */
static void test_raw_text_elements(void) {
    cwist_html_element_t *st = cwist_html_element_create("style");
    cwist_html_element_set_text(st, ".a > .b { content: \"x\" }");
    cwist_html_element_t *js = cwist_html_element_create("script");
    cwist_html_element_set_text(js, "if (a < b && c) { s = '</SCRIPT>'; }");
    cwist_html_element_t *p = cwist_html_element_create("p");
    cwist_html_element_set_text(p, "a < b");
    cwist_sstring *o1 = cwist_html_render(st), *o2 = cwist_html_render(js), *o3 = cwist_html_render(p);
    assert(strcmp(o1->data, "<style>.a > .b { content: \"x\" }</style>") == 0);
    assert(strcmp(o2->data, "<script>if (a < b && c) { s = '<\\/SCRIPT>'; }</script>") == 0);
    assert(strcmp(o3->data, "<p>a &lt; b</p>") == 0);
    cwist_sstring_destroy(o1);
    cwist_sstring_destroy(o2);
    cwist_sstring_destroy(o3);
    cwist_html_element_destroy(st);
    cwist_html_element_destroy(js);
    cwist_html_element_destroy(p);
    printf("  raw text elements OK\n");
}

static void set_path_cookies(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Set-Cookie", "admin=1; Path=/admin");
    cwist_http_header_add(&res->headers, "Set-Cookie", "site=1; Path=/");
    cwist_http_header_add(&res->headers, "Set-Cookie", "here=1");
}

static char seen_cookie[512];
static void echo_cookie(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    const char *c = cwist_http_header_get(req->headers, "Cookie");
    snprintf(seen_cookie, sizeof(seen_cookie), "%s", c ? c : "");
}

/* #995: the test client sends a cookie only to matching paths. */
static void test_client_cookie_paths(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/app/login", set_path_cookies);
    cwist_app_get(app, "/admin/x", echo_cookie);
    cwist_app_get(app, "/public", echo_cookie);
    cwist_app_get(app, "/app/page", echo_cookie);
    cwist_test_client *c = cwist_test_client_create(app);
    cwist_http_response_destroy(cwist_test_client_get(c, "/app/login"));
    cwist_http_response_destroy(cwist_test_client_get(c, "/public"));
    assert(strstr(seen_cookie, "site=1") && !strstr(seen_cookie, "admin=1") &&
           !strstr(seen_cookie, "here=1"));
    cwist_http_response_destroy(cwist_test_client_get(c, "/admin/x"));
    assert(strstr(seen_cookie, "admin=1") && strstr(seen_cookie, "site=1"));
    cwist_http_response_destroy(cwist_test_client_get(c, "/app/page"));
    assert(strstr(seen_cookie, "here=1") && !strstr(seen_cookie, "admin=1"));
    cwist_test_client_destroy(c);
    cwist_app_destroy(app);
    printf("  test client cookie paths OK\n");
}

/* #999: a detached multiport sub-app keeps the session settings and pools. */
static void test_multiport_subapp_settings(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "multiport-secret") == 0);
    cwist_app_set_session_name(app, "mp_sid");
    cwist_app_set_session_max_age(app, 123);
#ifdef CWIST_DB
    cwist_error_t perr = cwist_app_use_db_pool(app, ":memory:", 2);
    assert(cwist_error_is_ok(&perr));
#endif
    cwist_app *sub = cwist_multiport_get_app(&app, 18099);
    assert(sub != NULL);
    assert(sub->session_secret && strcmp(sub->session_secret, "multiport-secret") == 0);
    assert(sub->session_name && strcmp(sub->session_name, "mp_sid") == 0);
    assert(sub->session_max_age == 123);
#ifdef CWIST_DB
    assert(cwist_app_get_db_pool(sub) == cwist_app_get_db_pool(app));
    assert(cwist_app_get_db_pool(sub) != NULL);
#endif
    cwist_app_destroy(app);
    printf("  multiport sub-app settings OK\n");
}

#ifdef CWIST_DB
static int count_rows(sqlite3 *d) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(d, "SELECT count(*) FROM t", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int file_rows(const char *path) {
    sqlite3 *d = NULL;
    sqlite3_open(path, &d);
    int n = count_rows(d);
    sqlite3_close(d);
    return n;
}

/* #990: a deserialized image is written to the file and stays persistent. */
static void test_nuke_deserialize_persists(void) {
    char path[] = "/tmp/cwist_nuke_deser_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);
    sqlite3 *d = NULL;
    sqlite3_open(path, &d);
    sqlite3_exec(d, "CREATE TABLE t(x); INSERT INTO t VALUES(1);", 0, 0, 0);
    sqlite3_close(d);
    sqlite3 *src = NULL;
    sqlite3_open(":memory:", &src);
    sqlite3_exec(src, "CREATE TABLE t(x); INSERT INTO t VALUES(10),(11),(12),(13),(14);", 0, 0, 0);
    sqlite3_int64 sz = 0;
    unsigned char *img = sqlite3_serialize(src, "main", &sz, 0);
    sqlite3_close(src);
    assert(img && sz > 0);
    assert(cwist_nuke_init(path, 0) == 0);
    assert(cwist_nuke_deserialize(img, sz) == 0);
    assert(count_rows(cwist_nuke_get_db()) == 5);
    assert(file_rows(path) == 5);
    sqlite3_exec(cwist_nuke_get_db(), "INSERT INTO t VALUES(99);", 0, 0, 0);
    cwist_nuke_close();
    assert(file_rows(path) == 6);
    unlink(path);
    char wal[64];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    unlink(wal);
    snprintf(wal, sizeof(wal), "%s-shm", path);
    unlink(wal);
    printf("  nuke deserialize persists OK\n");
}
#endif

/* #992: available RAM includes reclaimable page cache. */
static void test_available_ram(void) {
    uint64_t avail = cwist_get_available_ram();
#ifdef __linux__
    assert(avail > 0);
#endif
    (void)avail;
    printf("  available ram OK\n");
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
#ifdef CWIST_JWT
    test_jwt_canonical_segments();
#endif
#ifdef CWIST_DB
    test_db_insert_healed_numbers();
#endif
    test_route_middleware_order();
    test_http_date_calendar();
    test_html_fragment_children();
    test_sstring_append_after_nul();
    test_css_declaration_checks();
    test_metrics_exposition();
#ifdef CWIST_DB
    test_db_statement_settings();
#endif
    test_json_heal_fuzzy_rename();
    test_redis_invalid_arguments();
    test_csrf_across_workers();
    test_htmx_boosted();
    test_raw_text_elements();
    test_client_cookie_paths();
    test_multiport_subapp_settings();
#ifdef CWIST_DB
    test_nuke_deserialize_persists();
#endif
    test_available_ram();
    printf("All regression batch tests passed.\n");
    return 0;
}
