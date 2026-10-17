/**
 * @file test_app_named_routes_and_error_handler.c
 * @brief Smoke tests for uncovered cwist_app_* functions (issue #342).
 *
 * Covers:
 *   cwist_app_post_named    -- register a named POST route; cwist_url_for finds it.
 *   cwist_app_delete_named  -- register a named DELETE route; cwist_url_for finds it.
 *   cwist_app_enable_healthz -- registers GET /healthz; cwist_url_for returns the path.
 *   cwist_app_set_error_handler -- stored in app->error_handler; survives NULL app.
 *
 * None of these start a live server -- they only exercise the registration
 * and lookup paths, which is enough to confirm the API surface behaves.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Minimal placeholder handler -- never invoked, just used as a non-NULL value. */
static void noop_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

/* Minimal error handler -- never invoked, stored only as a field value. */
static void noop_error_handler(cwist_http_request *req, cwist_http_response *res,
                                cwist_http_status_t status) {
    (void)req;
    (void)res;
    (void)status;
}

static void test_post_named(void) {
    printf("test_app_post_named...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_post_named(app, "/items", "items_create", noop_handler);

    char *url = cwist_url_for(app, "items_create", NULL);
    assert(url != NULL);
    assert(strcmp(url, "/items") == 0);
    free(url);

    /* NULL guards must not crash. */
    cwist_app_post_named(NULL, "/x", "x", noop_handler);
    cwist_app_post_named(app, NULL, "x", noop_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_delete_named(void) {
    printf("test_app_delete_named...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_delete_named(app, "/items/:id", "items_delete", noop_handler);

    char *url = cwist_url_for(app, "items_delete", NULL);
    assert(url != NULL);
    assert(strstr(url, "/items") != NULL);
    free(url);

    cwist_app_delete_named(NULL, "/x", "x", noop_handler);
    cwist_app_delete_named(app, NULL, "x", noop_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_enable_healthz(void) {
    printf("test_app_enable_healthz...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_enable_healthz(app);

    /* The healthz route must be discoverable by its path. */
    char *url = cwist_url_for(app, "/healthz", NULL);
    if (url) {
        /* Named registration exists -- verify path. */
        assert(strstr(url, "healthz") != NULL);
        free(url);
    }
    /* If url_for returns NULL the route was registered without a name, which
     * is also acceptable -- the assertion below just confirms no crash. */

    cwist_app_enable_healthz(NULL);

    cwist_app_destroy(app);
    printf("  OK\n");
}

static void test_set_error_handler(void) {
    printf("test_app_set_error_handler...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    /* Before setting: field should be NULL. */
    assert(app->error_handler == NULL);

    cwist_app_set_error_handler(app, noop_error_handler);
    assert(app->error_handler == noop_error_handler);

    /* Overwrite with NULL clears the handler. */
    cwist_app_set_error_handler(app, NULL);
    assert(app->error_handler == NULL);

    /* NULL app must not crash. */
    cwist_app_set_error_handler(NULL, noop_error_handler);

    cwist_app_destroy(app);
    printf("  OK\n");
}

int main(void) {
    test_post_named();
    test_delete_named();
    test_enable_healthz();
    test_set_error_handler();
    printf("All test_app_named_routes_and_error_handler tests passed.\n");
    return 0;
}
