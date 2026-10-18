/**
 * @file test_app_route_helpers.c
 * @brief Smoke tests for the cwist_app_* named/opt route helpers and
 *        cwist_app_set_session_max_age (issue #390). Registration and lookup
 *        paths only; no live server is started.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <cwist/net/http/session.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void noop_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

static void test_get_named(void) {
    printf("test_app_get_named...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_get_named(app, "/articles", "articles_list", noop_handler);

    char *url = cwist_url_for(app, "articles_list", NULL);
    assert(url != NULL);
    assert(strcmp(url, "/articles") == 0);
    free(url);

    cwist_app_get_named(NULL, "/x", "x", noop_handler);
    cwist_app_get_named(app, NULL, "x", noop_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_put_named(void) {
    printf("test_app_put_named...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_put_named(app, "/articles/:id", "articles_replace", noop_handler);

    char *url = cwist_url_for(app, "articles_replace", NULL);
    assert(url != NULL);
    assert(strstr(url, "/articles") != NULL);
    free(url);

    cwist_app_put_named(NULL, "/x", "x", noop_handler);
    cwist_app_put_named(app, NULL, "x", noop_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_patch_named(void) {
    printf("test_app_patch_named...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_patch_named(app, "/articles/:id", "articles_update", noop_handler);

    char *url = cwist_url_for(app, "articles_update", NULL);
    assert(url != NULL);
    assert(strstr(url, "/articles") != NULL);
    free(url);

    cwist_app_patch_named(NULL, "/x", "x", noop_handler);
    cwist_app_patch_named(app, NULL, "x", noop_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_get_opt(void) {
    printf("test_app_get_opt...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_endpoint_opt_t opts = 0;
    cwist_app_get_opt(app, "/data", noop_handler, opts);

    cwist_app_get_opt(NULL, "/data", noop_handler, opts);
    cwist_app_get_opt(app, NULL, noop_handler, opts);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_post_opt(void) {
    printf("test_app_post_opt...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_endpoint_opt_t opts = 0;
    cwist_app_post_opt(app, "/data", noop_handler, opts);

    cwist_app_post_opt(NULL, "/data", noop_handler, opts);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_put_opt(void) {
    printf("test_app_put_opt...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_endpoint_opt_t opts = 0;
    cwist_app_put_opt(app, "/data/:id", noop_handler, opts);

    cwist_app_put_opt(NULL, "/data/:id", noop_handler, opts);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_patch_opt(void) {
    printf("test_app_patch_opt...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_endpoint_opt_t opts = 0;
    cwist_app_patch_opt(app, "/data/:id", noop_handler, opts);

    cwist_app_patch_opt(NULL, "/data/:id", noop_handler, opts);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_delete_opt(void) {
    printf("test_app_delete_opt...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_endpoint_opt_t opts = 0;
    cwist_app_delete_opt(app, "/data/:id", noop_handler, opts);

    cwist_app_delete_opt(NULL, "/data/:id", noop_handler, opts);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_set_session_max_age(void) {
    printf("test_app_set_session_max_age...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_set_session_max_age(app, 3600);
    cwist_app_set_session_max_age(app, 0);

    /* NULL must not crash. */
    cwist_app_set_session_max_age(NULL, 3600);

    cwist_app_destroy(app);
    printf("  OK\n");
}

int main(void) {
    test_get_named();
    test_put_named();
    test_patch_named();
    test_get_opt();
    test_post_opt();
    test_put_opt();
    test_patch_opt();
    test_delete_opt();
    test_set_session_max_age();
    printf("All test_app_route_helpers tests passed.\n");
    return 0;
}
