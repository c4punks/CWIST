#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/assets.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/core/crypto/sha256.h>
#include <cwist/core/html/component.h>
#include <cwist/core/html/css_composer.h>
#include <cwist/net/http/html_response.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define APP_CSS "body{margin:0}"
#define APP_CSS_V2 "body{margin:4px}"

static bool ok(cwist_error_t err) {
    bool good = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return good;
}

/* First 16 hex digits of SHA-256, computed independently of assets.c. */
static void hash16(const char *data, char out[17]) {
    uint8_t digest[CWIST_SHA256_DIGEST_LEN];
    cwist_sha256_ctx ctx;
    cwist_sha256_init(&ctx);
    cwist_sha256_update(&ctx, (const uint8_t *)data, strlen(data));
    cwist_sha256_final(&ctx, digest);
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static cwist_http_response *get(cwist_test_client *client, const char *path,
                                const char *if_none_match) {
    cwist_test_client_kv headers[] = {{"If-None-Match", if_none_match}};
    cwist_test_client_request_options opts = {0};
    if (if_none_match) {
        opts.headers = headers;
        opts.header_count = 1;
    }
    cwist_http_response *res = cwist_test_client_request_ex(client, CWIST_HTTP_GET, path, &opts);
    assert(res != NULL);
    return res;
}

static const char *header(cwist_http_response *res, const char *name) {
    return cwist_http_header_get(res->headers, name);
}

static bool body_is(cwist_http_response *res, const char *expected) {
    size_t len = strlen(expected);
    if (res->is_ptr_body) {
        return res->ptr_body_len == len && memcmp(res->ptr_body, expected, len) == 0;
    }
    return res->body && res->body->data && strcmp(res->body->data, expected) == 0;
}

static void test_serving(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_asset_url(app, "css/app.css") == NULL);
    assert(ok(cwist_app_asset_add(app, "css/app.css", APP_CSS, strlen(APP_CSS), NULL)));

    char h[17], expected_url[128], etag[32];
    hash16(APP_CSS, h);
    snprintf(expected_url, sizeof(expected_url), "/assets/css/app.%s.css", h);
    snprintf(etag, sizeof(etag), "\"%s\"", h);
    const char *url = cwist_app_asset_url(app, "css/app.css");
    assert(url != NULL && strcmp(url, expected_url) == 0);

    cwist_test_client *client = cwist_test_client_create(app);
    cwist_http_response *res = get(client, url, NULL);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(body_is(res, APP_CSS));
    assert(strcmp(header(res, "Content-Type"), "text/css; charset=utf-8") == 0);
    assert(strcmp(header(res, "Cache-Control"), CWIST_ASSET_IMMUTABLE_CACHE) == 0);
    assert(strcmp(header(res, "ETag"), etag) == 0);
    cwist_http_response_destroy(res);

    /* Conditional requests: exact, weak, listed and wildcard validators. */
    char weak[40], listed[80];
    snprintf(weak, sizeof(weak), "W/%s", etag);
    snprintf(listed, sizeof(listed), "\"nope\", %s", etag);
    const char *matching[] = {etag, weak, listed, "*"};
    for (size_t i = 0; i < 4; i++) {
        res = get(client, url, matching[i]);
        assert(res->status_code == CWIST_HTTP_NOT_MODIFIED);
        assert(strcmp(header(res, "ETag"), etag) == 0);
        assert(strcmp(header(res, "Cache-Control"), CWIST_ASSET_IMMUTABLE_CACHE) == 0);
        assert(body_is(res, ""));
        cwist_http_response_destroy(res);
    }
    res = get(client, url, "\"0000000000000000\"");
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS));
    cwist_http_response_destroy(res);

    /* The logical name is served too, but must be revalidated. */
    res = get(client, "/assets/css/app.css", NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS));
    assert(strcmp(header(res, "Cache-Control"), "no-cache") == 0);
    assert(strcmp(header(res, "ETag"), etag) == 0);
    cwist_http_response_destroy(res);

    /* Identical content keeps its URL. */
    assert(ok(cwist_app_asset_add(app, "css/app.css", APP_CSS, strlen(APP_CSS), NULL)));
    assert(cwist_app_asset_url(app, "css/app.css") == url);

    /* New content gets a new URL; the old URL keeps serving the old bytes. */
    assert(ok(cwist_app_asset_add(app, "css/app.css", APP_CSS_V2, strlen(APP_CSS_V2), NULL)));
    const char *url_v2 = cwist_app_asset_url(app, "css/app.css");
    assert(url_v2 != NULL && strcmp(url_v2, expected_url) != 0);
    res = get(client, url_v2, NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS_V2));
    cwist_http_response_destroy(res);
    res = get(client, expected_url, NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS));
    cwist_http_response_destroy(res);
    res = get(client, "/assets/css/app.css", NULL);
    assert(body_is(res, APP_CSS_V2));
    cwist_http_response_destroy(res);

    /* Going back to the first content points the name at it again. */
    assert(ok(cwist_app_asset_add(app, "css/app.css", APP_CSS, strlen(APP_CSS), NULL)));
    assert(strcmp(cwist_app_asset_url(app, "css/app.css"), expected_url) == 0);
    res = get(client, "/assets/css/app.css", NULL);
    assert(body_is(res, APP_CSS));
    cwist_http_response_destroy(res);

    /* Unknown names, other prefixes and non-GET methods are not assets. */
    res = get(client, "/assets/css/app.0000000000000000.css", NULL);
    assert(res->status_code == CWIST_HTTP_NOT_FOUND);
    cwist_http_response_destroy(res);
    res = get(client, "/assetsx/css/app.css", NULL);
    assert(res->status_code == CWIST_HTTP_NOT_FOUND);
    cwist_http_response_destroy(res);
    res = cwist_test_client_post(client, url, "x");
    assert(res != NULL && res->status_code == CWIST_HTTP_NOT_FOUND);
    cwist_http_response_destroy(res);

    /* The prefix cannot move once URLs were handed out. */
    assert(!ok(cwist_app_asset_prefix(app, "/static")));

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed test_serving\n");
}

static void test_names_and_types(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    const char *bad[] = {"",           "/a.css",  "a/",     "a//b.css", "../a.css", "a/./b.css",
                         "a/../b.css", "a b.css", "a?.css", "a%20.css", "a\\b.css"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!ok(cwist_app_asset_add(app, bad[i], "x", 1, NULL)));
    }
    assert(!ok(cwist_app_asset_add(app, NULL, "x", 1, NULL)));
    assert(!ok(cwist_app_asset_add(NULL, "a.css", "x", 1, NULL)));
    assert(!ok(cwist_app_asset_add(app, "a.css", NULL, 1, NULL)));
    assert(!ok(cwist_app_asset_add(app, "a.css", "x", 1, "text/css\r\nX-Evil: 1")));
    assert(!ok(cwist_app_asset_add(app, "a.css", "x", 1, "")));

    char h[17];
    hash16("x", h);
    struct {
        const char *name;
        const char *hashed_suffix; /* after "/assets/" */
    } cases[] = {
        {"LICENSE", "LICENSE.%s"},         {"lib.v2/app", "lib.v2/app.%s"},
        {"a.min.js", "a.min.%s.js"},       {".well-known/x.txt", ".well-known/x.%s.txt"},
        {"img/.hidden", "img/.hidden.%s"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(ok(cwist_app_asset_add(app, cases[i].name, "x", 1, NULL)));
        char expected[128] = "/assets/";
        snprintf(expected + 8, sizeof(expected) - 8, cases[i].hashed_suffix, h);
        assert(strcmp(cwist_app_asset_url(app, cases[i].name), expected) == 0);
    }

    /* Content types from the extension (case-insensitive), or as given. */
    assert(ok(cwist_app_asset_add(app, "f/font.WOFF2", "x", 1, NULL)));
    assert(ok(cwist_app_asset_add(app, "blob.bin", "x", 1, NULL)));
    assert(ok(cwist_app_asset_add(app, "data.cfg", "x", 1, "application/toml")));
    assert(ok(cwist_app_asset_add(app, "empty.txt", NULL, 0, NULL)));
    cwist_test_client *client = cwist_test_client_create(app);
    const struct {
        const char *path;
        const char *type;
    } types[] = {{"/assets/f/font.WOFF2", "font/woff2"},
                 {"/assets/blob.bin", "application/octet-stream"},
                 {"/assets/data.cfg", "application/toml"},
                 {"/assets/empty.txt", "text/plain; charset=utf-8"}};
    for (size_t i = 0; i < 4; i++) {
        cwist_http_response *res = get(client, types[i].path, NULL);
        assert(res->status_code == CWIST_HTTP_OK);
        assert(strcmp(header(res, "Content-Type"), types[i].type) == 0);
        cwist_http_response_destroy(res);
    }
    cwist_http_response *res = get(client, "/assets/empty.txt", NULL);
    assert(body_is(res, ""));
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed test_names_and_types\n");
}

static void test_prefix_and_file(void) {
    cwist_app *app = cwist_app_create();
    assert(!ok(cwist_app_asset_prefix(app, "relative")));
    assert(!ok(cwist_app_asset_prefix(app, "/a b")));
    assert(!ok(cwist_app_asset_prefix(NULL, "/x")));
    assert(ok(cwist_app_asset_prefix(app, "/static/build/")));

    char path[] = "/tmp/cwist_asset_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, APP_CSS, strlen(APP_CSS)) == (ssize_t)strlen(APP_CSS));
    close(fd);
    assert(ok(cwist_app_asset_add_file(app, "app.css", path, NULL)));
    assert(!ok(cwist_app_asset_add_file(app, "missing.css", "/nonexistent/cwist.css", NULL)));
    unlink(path);

    char h[17], expected[64];
    hash16(APP_CSS, h);
    snprintf(expected, sizeof(expected), "/static/build/app.%s.css", h);
    assert(strcmp(cwist_app_asset_url(app, "app.css"), expected) == 0);
    cwist_test_client *client = cwist_test_client_create(app);
    cwist_http_response *res = get(client, expected, NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS));
    cwist_http_response_destroy(res);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);

    /* "/" serves assets at the root. */
    app = cwist_app_create();
    assert(ok(cwist_app_asset_prefix(app, "/")));
    assert(ok(cwist_app_asset_add(app, "app.css", APP_CSS, strlen(APP_CSS), NULL)));
    snprintf(expected, sizeof(expected), "/app.%s.css", h);
    assert(strcmp(cwist_app_asset_url(app, "app.css"), expected) == 0);
    client = cwist_test_client_create(app);
    res = get(client, expected, NULL);
    assert(res->status_code == CWIST_HTTP_OK);
    cwist_http_response_destroy(res);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed test_prefix_and_file\n");
}

static void tag_middleware(cwist_http_request *req, cwist_http_response *res,
                           cwist_handler_func next) {
    cwist_http_header_add(&res->headers, "X-Seen", "1");
    next(req, res);
}

static void test_middleware_and_subapp(void) {
    cwist_app *root = cwist_app_create();
    root->port = 18180;
    cwist_app_use(root, tag_middleware);
    assert(ok(cwist_app_asset_add(root, "app.css", APP_CSS, strlen(APP_CSS), NULL)));
    const char *url = cwist_app_asset_url(root, "app.css");

    /* Assets go through the app's middleware chain like static files. */
    cwist_test_client *client = cwist_test_client_create(root);
    cwist_http_response *res = get(client, url, NULL);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(header(res, "X-Seen") != NULL);
    cwist_http_response_destroy(res);

    /* A detached port gets a copy of the assets registered so far. */
    cwist_app *sub = cwist_multiport_get_app(&root, 18181);
    assert(sub != NULL);
    assert(strcmp(cwist_app_asset_url(sub, "app.css"), url) == 0);
    assert(ok(cwist_app_asset_add(root, "late.css", "a{}", 3, NULL)));
    assert(cwist_app_asset_url(sub, "late.css") == NULL);
    cwist_test_client *sub_client = cwist_test_client_create(sub);
    res = get(sub_client, url, NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, APP_CSS));
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(sub_client);
    cwist_test_client_destroy(client);
    cwist_app_destroy(root);
    printf("Passed test_middleware_and_subapp\n");
}

/* Phases 2-4 together: scoped CSS bundled, minified and published, and the
 * layout linking it by its content-hashed URL. */
static cwist_app *g_app;

static cwist_html_element_t *layout_render(const void *props, cwist_html_element_t **children,
                                           size_t child_count) {
    (void)props;
    cwist_html_element_t *html = cwist_html_element_create("html");
    if (!html) {
        for (size_t i = 0; i < child_count; i++) cwist_html_element_destroy(children[i]);
        return NULL;
    }
    cwist_html_element_t *link = cwist_html_element_create("link");
    cwist_html_element_add_attr(link, "rel", "stylesheet");
    cwist_html_element_add_attr(link, "href", cwist_app_asset_url(g_app, "app.css"));
    cwist_html_element_add_child(html, link);
    for (size_t i = 0; i < child_count; i++) cwist_html_element_add_child(html, children[i]);
    return html;
}

static cwist_css_scope g_scope;
static cwist_html_component_t *g_layout;

static void page_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_html_element_t *card = cwist_html_element_create("div");
    cwist_html_element_add_class(card, cwist_css_scope_class(&g_scope, "card"));
    if (cwist_http_response_set_view(req, res, card, g_layout, NULL) != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void test_pipeline(void) {
    g_app = cwist_app_create();
    cwist_css_scope_init(&g_scope, "card");
    assert(cwist_css_scope_add_rule(&g_scope, "card", "padding : 8px ;") == 0);
    assert(cwist_css_scope_class(&g_scope, "card") != NULL);
    cwist_sstring *scoped = cwist_css_scope_generate_stylesheet(&g_scope);
    const char *parts[] = {"/* base */\nbody {\n  margin: 0;\n}\n", scoped->data};
    cwist_sstring *bundle = cwist_css_bundle(parts, 2, true);
    assert(bundle != NULL);
    assert(strcmp(bundle->data, "body{margin:0}.card-8827595f{padding :8px}") == 0);
    assert(ok(cwist_app_asset_add(g_app, "app.css", bundle->data, bundle->size, NULL)));
    cwist_sstring_destroy(bundle);
    cwist_sstring_destroy(scoped);

    g_layout = cwist_html_component_create("layout", layout_render);
    cwist_app_get(g_app, "/", page_handler);
    cwist_test_client *client = cwist_test_client_create(g_app);

    char h[17], expected[256];
    hash16("body{margin:0}.card-8827595f{padding :8px}", h);
    snprintf(expected, sizeof(expected),
             "<!DOCTYPE html><html><link rel=\"stylesheet\" href=\"/assets/app.%s.css\">"
             "<div class=\"card-8827595f\"></div></html>",
             h);
    cwist_http_response *res = get(client, "/", NULL);
    assert(res->status_code == CWIST_HTTP_OK && body_is(res, expected));
    cwist_http_response_destroy(res);

    snprintf(expected, sizeof(expected), "/assets/app.%s.css", h);
    res = get(client, expected, NULL);
    assert(res->status_code == CWIST_HTTP_OK &&
           body_is(res, "body{margin:0}.card-8827595f{padding :8px}"));
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(client);
    cwist_html_component_destroy(g_layout);
    cwist_css_scope_destroy(&g_scope);
    cwist_app_destroy(g_app);
    printf("Passed test_pipeline\n");
}

int main(void) {
    printf("Testing content-hashed assets...\n");
    test_serving();
    test_names_and_types();
    test_prefix_and_file();
    test_middleware_and_subapp();
    test_pipeline();
    printf("All asset tests passed!\n");
    return 0;
}
