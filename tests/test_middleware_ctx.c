/**
 * @file test_middleware_ctx.c
 * @brief Extended middleware with a user context: cwist_app_use_ex().
 *
 * Requests go through the in-process test client, which dispatches through
 * the same routing and middleware code as every network transport.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Always evaluated, unlike assert(): several checks register middleware. */
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                        \
        }                                                                   \
    } while (0)

typedef struct {
    char before;
    char after;
    int calls;
    int destroyed;
} mw_ctx;

/* Execution trace: each middleware appends its tag before and after next(). */
static char g_trace[64];

static void trace(char c) {
    size_t n = strlen(g_trace);
    if (n + 1 < sizeof(g_trace)) {
        g_trace[n] = c;
        g_trace[n + 1] = '\0';
    }
}

static void ctx_destroy(void *user_ctx) {
    ((mw_ctx *)user_ctx)->destroyed++;
}

static void tracing_mw(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next,
                       void *user_ctx) {
    mw_ctx *ctx = (mw_ctx *)user_ctx;
    ctx->calls++;
    trace(ctx->before);
    if (next) next(req, res);
    trace(ctx->after);
}

static void legacy_mw(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    trace('l');
    if (next) next(req, res);
    trace('L');
}

/* Short-circuits /blocked; elsewhere it edits the response after next(). */
static void gate_mw(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next,
                    void *user_ctx) {
    mw_ctx *ctx = (mw_ctx *)user_ctx;
    ctx->calls++;
    if (req->path && req->path->data && strcmp(req->path->data, "/blocked") == 0) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "blocked");
        return;
    }
    if (next) next(req, res);
    cwist_http_header_add(&res->headers, "X-After", "yes");
    res->status_code = CWIST_HTTP_ACCEPTED;
}

static int g_handler_calls = 0;
static void handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    g_handler_calls++;
    trace('H');
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "handler");
}

static bool registered(cwist_error_t err) {
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

static void expect_body(cwist_http_response *res, int status, const char *body) {
    REQUIRE(res != NULL);
    REQUIRE((int)res->status_code == status);
    if (body) {
        REQUIRE(res->body && res->body->data);
        if (strcmp(res->body->data, body) != 0) {
            fprintf(stderr, "FAIL: body \"%s\", want \"%s\"\n", res->body->data, body);
            REQUIRE(0);
        }
    }
    cwist_http_response_destroy(res);
}

static void expect_trace(const char *want) {
    if (strcmp(g_trace, want) != 0) {
        fprintf(stderr, "FAIL: trace \"%s\", want \"%s\"\n", g_trace, want);
        REQUIRE(0);
    }
    g_trace[0] = '\0';
}

static void test_order(void) {
    mw_ctx a = {'a', 'A', 0, 0}, b = {'b', 'B', 0, 0};
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_use_ex(app, NULL, tracing_mw, &a, ctx_destroy)));
    cwist_app_use(app, legacy_mw);
    REQUIRE(registered(cwist_app_use_ex(app, NULL, tracing_mw, &b, ctx_destroy)));
    cwist_app_get(app, "/r", handler);
    cwist_test_client *client = cwist_test_client_create(app);

    g_trace[0] = '\0';
    expect_body(cwist_test_client_get(client, "/r"), 200, "handler");
    expect_trace("albHBLA");
    expect_body(cwist_test_client_get(client, "/r"), 200, "handler");
    expect_trace("albHBLA");
    REQUIRE(a.calls == 2 && b.calls == 2);

    cwist_test_client_destroy(client);
    REQUIRE(a.destroyed == 0 && b.destroyed == 0);
    cwist_app_destroy(app);
    REQUIRE(a.destroyed == 1 && b.destroyed == 1);
    printf("Passed extended and legacy middleware run in registration order\n");
}

static void test_short_circuit_and_post_next(void) {
    mw_ctx gate = {0, 0, 0, 0};
    g_handler_calls = 0;
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_use_ex(app, NULL, gate_mw, &gate, ctx_destroy)));
    cwist_app_get(app, "/ok", handler);
    cwist_app_get(app, "/blocked", handler);
    cwist_test_client *client = cwist_test_client_create(app);

    /* Changes made after next() returns are in the response. */
    cwist_http_response *res = cwist_test_client_get(client, "/ok");
    REQUIRE(res);
    const char *after = cwist_http_header_get(res->headers, "X-After");
    REQUIRE(after && strcmp(after, "yes") == 0);
    expect_body(res, 202, "handler");

    /* Not calling next() keeps the handler from running. */
    expect_body(cwist_test_client_get(client, "/blocked"), 403, "blocked");
    REQUIRE(gate.calls == 2 && g_handler_calls == 1);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(gate.destroyed == 1);
    g_trace[0] = '\0';
    printf("Passed extended middleware can short-circuit and edit after next\n");
}

static void test_failures_release_context(void) {
    mw_ctx ctx = {'x', 'X', 0, 0};
    cwist_app *app = cwist_app_create();

    REQUIRE(!registered(cwist_app_use_ex(NULL, NULL, tracing_mw, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 1);
    REQUIRE(!registered(cwist_app_use_ex(app, NULL, NULL, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 2);
    /* Exactly one of the two function pointers may be set. */
    REQUIRE(!registered(cwist_app_use_ex(app, legacy_mw, tracing_mw, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 3);
    /* Without a destructor the caller keeps the context; nothing to call. */
    REQUIRE(!registered(cwist_app_use_ex(NULL, NULL, tracing_mw, &ctx, NULL)));
    REQUIRE(ctx.destroyed == 3);

    /* The failed registrations left no middleware behind. */
    cwist_app_get(app, "/r", handler);
    cwist_test_client *client = cwist_test_client_create(app);
    g_trace[0] = '\0';
    expect_body(cwist_test_client_get(client, "/r"), 200, "handler");
    expect_trace("H");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(ctx.destroyed == 3 && ctx.calls == 0);
    printf("Passed failed registrations release the context\n");
}

static void test_caller_owned(void) {
    mw_ctx ctx = {'c', 'C', 0, 0};
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_use_ex(app, NULL, tracing_mw, &ctx, NULL)));
    cwist_app_get(app, "/r", handler);
    cwist_test_client *client = cwist_test_client_create(app);
    g_trace[0] = '\0';
    expect_body(cwist_test_client_get(client, "/r"), 200, "handler");
    expect_trace("cHC");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(ctx.calls == 1 && ctx.destroyed == 0);
    printf("Passed NULL destructor leaves ownership with the caller\n");
}

static void test_multiport_shares_context(void) {
    mw_ctx ctx = {'m', 'M', 0, 0};
    cwist_app *root = cwist_app_create();
    root->port = 18080;
    REQUIRE(registered(cwist_app_use_ex(root, NULL, tracing_mw, &ctx, ctx_destroy)));
    cwist_app_get(root, "/r", handler);

    /* The sub-app runs the same extended middleware with the same context. */
    cwist_app *sub = cwist_multiport_get_app(&root, 18081);
    REQUIRE(sub && sub != root);
    cwist_test_client *sub_client = cwist_test_client_create(sub);
    g_trace[0] = '\0';
    expect_body(cwist_test_client_get(sub_client, "/r"), 200, "handler");
    expect_trace("mHM");
    cwist_test_client_destroy(sub_client);

    cwist_test_client *root_client = cwist_test_client_create(root);
    expect_body(cwist_test_client_get(root_client, "/r"), 200, "handler");
    expect_trace("mHM");
    cwist_test_client_destroy(root_client);

    /* Only the root owns the context: it is released exactly once. */
    cwist_app_destroy(root);
    REQUIRE(ctx.calls == 2 && ctx.destroyed == 1);
    printf("Passed multiport sub-apps share the middleware context without owning it\n");
}

static void test_heap_context(void) {
    /* A heap context released by cwist_free: ASan/LSan catch a leak or a
     * double release here. */
    mw_ctx *ctx = (mw_ctx *)cwist_alloc(sizeof(*ctx));
    REQUIRE(ctx);
    ctx->before = 'h';
    ctx->after = 'h';
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_use_ex(app, NULL, tracing_mw, ctx, cwist_free)));
    cwist_app_get(app, "/r", handler);
    cwist_test_client *client = cwist_test_client_create(app);
    g_trace[0] = '\0';
    expect_body(cwist_test_client_get(client, "/r"), 200, "handler");
    expect_trace("hHh");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed heap context released with the app\n");
}

int main(void) {
    printf("Testing extended middleware user context...\n");
    test_order();
    test_short_circuit_and_post_next();
    test_failures_release_context();
    test_caller_owned();
    test_multiport_shares_context();
    test_heap_context();
    printf("All extended middleware tests passed.\n");
    return 0;
}
