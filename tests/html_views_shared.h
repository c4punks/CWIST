/*
 * One set of HTML views (components, scoped CSS, a hashed stylesheet asset,
 * page/fragment/out-of-band/redirect routes) and one checker, compiled into
 * both the native test (tests/test_html_parity.c) and the Emscripten smoke
 * test (tests/wasm_smoke.c). Both builds dispatch the same raw HTTP requests
 * through cwist_app_dispatch_memory() and compare every response against the
 * same literal bytes below, so a pass on both proves the WASM build renders
 * exactly what the server renders, through the same code.
 */
#ifndef CWIST_TESTS_HTML_VIEWS_SHARED_H
#define CWIST_TESTS_HTML_VIEWS_SHARED_H

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/assets.h>
#include <cwist/core/html/component.h>
#include <cwist/core/html/css_composer.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/html_response.h>
#include <stdio.h>
#include <string.h>

/* sha256("body{margin:0;font-family:sans-serif}.card-8827595f{...}") starts
 * with 9306adcbf206953d; the scope suffix is FNV-1a("card"). */
#define HTML_VIEWS_CSS \
    "body{margin:0;font-family:sans-serif}.card-8827595f{padding:8px;border:1px solid #ccc}"
#define HTML_VIEWS_CSS_URL "/assets/app.9306adcbf206953d.css"
#define HTML_VIEWS_CARDS                                                             \
    "<section id=\"cards\">"                                                         \
    "<article class=\"card-8827595f\"><h2>One</h2><p>first &amp; best</p></article>" \
    "<article class=\"card-8827595f\"><h2>Two</h2><p>second</p></article>"           \
    "</section>"
#define HTML_VIEWS_PAGE                                                       \
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Cards</title>" \
    "<link rel=\"stylesheet\" href=\"" HTML_VIEWS_CSS_URL "\"></head>"        \
    "<body><main id=\"content\">" HTML_VIEWS_CARDS "</main></body></html>"
#define HTML_VIEWS_FRAGMENT HTML_VIEWS_CARDS "<span id=\"count\" hx-swap-oob=\"true\">2</span>"

typedef struct {
    const char *title;
    const char *body;
} html_views_card_props;

typedef struct {
    cwist_app *app;
    const char *title;
} html_views_layout_props;

static cwist_css_scope html_views_scope;
static cwist_html_component_t *html_views_card;
static cwist_html_component_t *html_views_layout;

static cwist_html_element_t *html_views_text(const char *tag, const char *text) {
    cwist_html_element_t *el = cwist_html_element_create(tag);
    if (el) cwist_html_element_set_text(el, text);
    return el;
}

static cwist_html_element_t *
html_views_card_render(const void *props, cwist_html_element_t **children, size_t child_count) {
    const html_views_card_props *p = (const html_views_card_props *)props;
    for (size_t i = 0; i < child_count; i++) cwist_html_element_destroy(children[i]);
    cwist_html_element_t *article = cwist_html_element_create("article");
    if (!article) return NULL;
    cwist_html_element_add_class(article, cwist_css_scope_class(&html_views_scope, "card"));
    cwist_html_element_add_child(article, html_views_text("h2", p->title));
    cwist_html_element_add_child(article, html_views_text("p", p->body));
    return article;
}

static cwist_html_element_t *
html_views_layout_render(const void *props, cwist_html_element_t **children, size_t child_count) {
    const html_views_layout_props *p = (const html_views_layout_props *)props;
    cwist_html_element_t *html = cwist_html_element_create("html");
    if (!html) {
        for (size_t i = 0; i < child_count; i++) cwist_html_element_destroy(children[i]);
        return NULL;
    }
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *meta = cwist_html_element_create("meta");
    cwist_html_element_add_attr(meta, "charset", "utf-8");
    cwist_html_element_add_child(head, meta);
    cwist_html_element_add_child(head, html_views_text("title", p->title));
    cwist_html_element_t *link = cwist_html_element_create("link");
    cwist_html_element_add_attr(link, "rel", "stylesheet");
    cwist_html_element_add_attr(link, "href", cwist_app_asset_url(p->app, "app.css"));
    cwist_html_element_add_child(head, link);
    cwist_html_element_add_child(html, head);

    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *main_el = cwist_html_element_create("main");
    cwist_html_element_set_id(main_el, "content");
    for (size_t i = 0; i < child_count; i++) cwist_html_element_add_child(main_el, children[i]);
    cwist_html_element_add_child(body, main_el);
    cwist_html_element_add_child(html, body);
    return html;
}

static void html_views_cards_handler(cwist_http_request *req, cwist_http_response *res) {
    static const html_views_card_props cards[] = {{"One", "first & best"}, {"Two", "second"}};
    cwist_html_element_t *section = cwist_html_element_create("section");
    cwist_html_element_set_id(section, "cards");
    for (size_t i = 0; i < 2; i++) {
        cwist_html_element_add_child(
            section, cwist_html_component_instantiate(html_views_card, &cards[i], NULL, 0));
    }
    html_views_layout_props layout = {req->app, "Cards"};
    int rc = cwist_http_response_set_view(req, res, section, html_views_layout, &layout);
    cwist_html_element_t *count = html_views_text("span", "2");
    cwist_html_element_set_id(count, "count");
    rc |= cwist_http_response_add_oob(req, res, count);
    if (rc != 0) res->status_code = CWIST_HTTP_INTERNAL_ERROR;
}

static void html_views_add_handler(cwist_http_request *req, cwist_http_response *res) {
    if (cwist_http_response_html_redirect(req, res, "/cards") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

/** Build the views and register them on `app`. 0 on success. */
static int html_views_install(cwist_app *app) {
    cwist_css_scope_init(&html_views_scope, "card");
    if (cwist_css_scope_add_rule(&html_views_scope, "card",
                                 "padding: 8px; border: 1px solid #ccc;") != 0 ||
        !cwist_css_scope_class(&html_views_scope, "card")) {
        return -1;
    }
    cwist_sstring *scoped = cwist_css_scope_generate_stylesheet(&html_views_scope);
    if (!scoped) return -1;
    const char *parts[] = {"body {\n  margin: 0;\n  font-family: sans-serif;\n}\n", scoped->data};
    cwist_sstring *bundle = cwist_css_bundle(parts, 2, true);
    cwist_sstring_destroy(scoped);
    if (!bundle) return -1;
    cwist_error_t err = cwist_app_asset_add(app, "app.css", bundle->data, bundle->size, NULL);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    cwist_sstring_destroy(bundle);
    if (!ok) return -1;

    html_views_card = cwist_html_component_create("card", html_views_card_render);
    html_views_layout = cwist_html_component_create("layout", html_views_layout_render);
    if (!html_views_card || !html_views_layout) return -1;
    cwist_app_get(app, "/cards", html_views_cards_handler);
    cwist_app_post(app, "/cards", html_views_add_handler);
    return 0;
}

static void html_views_teardown(void) {
    cwist_html_component_destroy(html_views_card);
    cwist_html_component_destroy(html_views_layout);
    html_views_card = NULL;
    html_views_layout = NULL;
    cwist_css_scope_destroy(&html_views_scope);
}

/* Dispatch one raw request and check status line, headers and exact body. */
static int html_views_expect(cwist_app *app, const char *request, const char *status_line,
                             const char *const *headers, size_t header_count, const char *body) {
    char *out = NULL;
    size_t out_len = 0;
    if (cwist_app_dispatch_memory(app, request, strlen(request), &out, &out_len) != 0 || !out) {
        fprintf(stderr, "html_views: dispatch failed for:\n%s\n", request);
        return -1;
    }
    int rc = 0;
    const char *split = NULL;
    for (size_t i = 0; i + 3 < out_len; i++) {
        if (memcmp(out + i, "\r\n\r\n", 4) == 0) {
            split = out + i;
            break;
        }
    }
    size_t head_len = split ? (size_t)(split - out) + 2 : 0; /* keep the last CRLF */
    if (!split || strncmp(out, status_line, strlen(status_line)) != 0) rc = -1;
    for (size_t i = 0; rc == 0 && i < header_count; i++) {
        char needle[256];
        snprintf(needle, sizeof(needle), "\r\n%s\r\n", headers[i]);
        size_t nlen = strlen(needle);
        bool found = false;
        for (size_t j = 0; j + nlen <= head_len; j++) {
            if (memcmp(out + j, needle, nlen) == 0) {
                found = true;
                break;
            }
        }
        if (!found) rc = -1;
    }
    if (rc == 0 && body) {
        const char *got = split + 4;
        size_t got_len = out_len - (size_t)(got - out);
        if (got_len != strlen(body) || memcmp(got, body, got_len) != 0) rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "html_views: unexpected response to:\n%s\n--- got ---\n%.*s\n", request,
                (int)out_len, out);
    }
    cwist_free(out);
    return rc;
}

/** Run every check. Returns 0, or the number of the first failing check. */
static int html_views_check(cwist_app *app) {
    static const char *const html_headers[] = {
        "Content-Type: text/html; charset=utf-8",
        "Vary: " CWIST_HTML_FRAGMENT_VARY,
    };
    static const char *const css_headers[] = {
        "Content-Type: text/css; charset=utf-8",
        "Cache-Control: " CWIST_ASSET_IMMUTABLE_CACHE,
        "ETag: \"9306adcbf206953d\"",
    };
    static const char *const see_other[] = {"Location: /cards"};
    static const char *const hx_redirect[] = {"HX-Redirect: /cards"};

    if (!cwist_app_asset_url(app, "app.css") ||
        strcmp(cwist_app_asset_url(app, "app.css"), HTML_VIEWS_CSS_URL) != 0) {
        fprintf(stderr, "html_views: asset url %s\n", cwist_app_asset_url(app, "app.css"));
        return 1;
    }
    if (html_views_expect(app, "GET /cards HTTP/1.1\r\nHost: t\r\n\r\n", "HTTP/1.1 200",
                          html_headers, 2, HTML_VIEWS_PAGE) != 0) {
        return 2;
    }
    if (html_views_expect(app, "GET /cards HTTP/1.1\r\nHost: t\r\nHX-Request: true\r\n\r\n",
                          "HTTP/1.1 200", html_headers, 2, HTML_VIEWS_FRAGMENT) != 0) {
        return 3;
    }
    if (html_views_expect(app, "GET /cards HTTP/1.1\r\nHost: t\r\nTurbo-Frame: cards\r\n\r\n",
                          "HTTP/1.1 200", html_headers, 2, HTML_VIEWS_FRAGMENT) != 0) {
        return 4;
    }
    if (html_views_expect(app, "GET " HTML_VIEWS_CSS_URL " HTTP/1.1\r\nHost: t\r\n\r\n",
                          "HTTP/1.1 200", css_headers, 3, HTML_VIEWS_CSS) != 0) {
        return 5;
    }
    if (html_views_expect(app,
                          "GET " HTML_VIEWS_CSS_URL " HTTP/1.1\r\nHost: t\r\n"
                          "If-None-Match: \"9306adcbf206953d\"\r\n\r\n",
                          "HTTP/1.1 304", NULL, 0, NULL) != 0) {
        return 6;
    }
    if (html_views_expect(app, "POST /cards HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n",
                          "HTTP/1.1 303", see_other, 1, "") != 0) {
        return 7;
    }
    if (html_views_expect(app,
                          "POST /cards HTTP/1.1\r\nHost: t\r\nHX-Request: true\r\n"
                          "Content-Length: 0\r\n\r\n",
                          "HTTP/1.1 200", hx_redirect, 1, "") != 0) {
        return 8;
    }
    return 0;
}

#endif
