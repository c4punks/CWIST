/**
 * @file test_route_ctx.c
 * @brief Per-route user context: cwist_app_{get,post,put,delete,patch}_ex().
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

/* Always evaluated, unlike assert(): several checks register routes. */
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                        \
        }                                                                   \
    } while (0)

typedef struct {
    const char *tag;
    int calls;
    int destroyed;
} route_ctx;

static void ctx_destroy(void *user_ctx) {
    ((route_ctx *)user_ctx)->destroyed++;
}

static int g_null_ctx_destroyed = 0;
static void null_ctx_destroy(void *user_ctx) {
    REQUIRE(user_ctx == NULL);
    g_null_ctx_destroyed++;
}

/* Replies with the context's tag, plus ":<id>" for routes with an :id param. */
static void tag_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    route_ctx *ctx = (route_ctx *)user_ctx;
    ctx->calls++;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, ctx->tag);
    const char *id = req->path_params ? cwist_query_map_get(req->path_params, "id") : NULL;
    if (id) {
        cwist_sstring_append(res->body, ":");
        cwist_sstring_append(res->body, id);
    }
}

static void null_ctx_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, user_ctx == NULL ? "null-ctx" : "unexpected");
}

static void plain_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "plain");
}

static int g_mw_calls = 0;
static void gate_middleware(cwist_http_request *req, cwist_http_response *res,
                            cwist_handler_func next) {
    g_mw_calls++;
    cwist_http_header_add(&res->headers, "X-Gate", "seen");
    if (req->path && req->path->data && strcmp(req->path->data, "/blocked") == 0) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        return;
    }
    if (next) next(req, res);
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

static void test_every_method(void) {
    route_ctx get = {"get", 0, 0}, post = {"post", 0, 0}, put = {"put", 0, 0};
    route_ctx del = {"delete", 0, 0}, patch = {"patch", 0, 0};
    cwist_app *app = cwist_app_create();
    REQUIRE(app);
    REQUIRE(registered(cwist_app_get_ex(app, "/r", tag_handler, &get, ctx_destroy)));
    REQUIRE(registered(cwist_app_post_ex(app, "/r", tag_handler, &post, ctx_destroy)));
    REQUIRE(registered(cwist_app_put_ex(app, "/r", tag_handler, &put, ctx_destroy)));
    REQUIRE(registered(cwist_app_delete_ex(app, "/r", tag_handler, &del, ctx_destroy)));
    REQUIRE(registered(cwist_app_patch_ex(app, "/r", tag_handler, &patch, ctx_destroy)));

    cwist_test_client *client = cwist_test_client_create(app);
    REQUIRE(client);
    expect_body(cwist_test_client_get(client, "/r"), 200, "get");
    expect_body(cwist_test_client_post(client, "/r", "x"), 200, "post");
    expect_body(cwist_test_client_put(client, "/r", "x"), 200, "put");
    expect_body(cwist_test_client_delete(client, "/r"), 200, "delete");
    expect_body(cwist_test_client_patch(client, "/r", "x"), 200, "patch");
    expect_body(cwist_test_client_get(client, "/r"), 200, "get");
    cwist_test_client_destroy(client);

    REQUIRE(get.calls == 2 && post.calls == 1 && put.calls == 1);
    REQUIRE(del.calls == 1 && patch.calls == 1);
    /* Nothing is released while the app is alive. */
    REQUIRE(get.destroyed + post.destroyed + put.destroyed + del.destroyed + patch.destroyed == 0);

    cwist_app_destroy(app);
    REQUIRE(get.destroyed == 1 && post.destroyed == 1 && put.destroyed == 1);
    REQUIRE(del.destroyed == 1 && patch.destroyed == 1);
    printf("Passed every method passes its own context\n");
}

static void test_param_route(void) {
    route_ctx user = {"user", 0, 0};
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_get_ex(app, "/users/:id", tag_handler, &user, ctx_destroy)));
    cwist_test_client *client = cwist_test_client_create(app);
    expect_body(cwist_test_client_get(client, "/users/42"), 200, "user:42");
    expect_body(cwist_test_client_get(client, "/users/7"), 200, "user:7");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(user.calls == 2 && user.destroyed == 1);
    printf("Passed :param route receives its context\n");
}

static void test_middleware(void) {
    route_ctx ok = {"ok", 0, 0}, blocked = {"blocked", 0, 0};
    g_mw_calls = 0;
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, gate_middleware);
    REQUIRE(registered(cwist_app_get_ex(app, "/ok", tag_handler, &ok, ctx_destroy)));
    REQUIRE(registered(cwist_app_get_ex(app, "/blocked", tag_handler, &blocked, ctx_destroy)));
    cwist_test_client *client = cwist_test_client_create(app);

    cwist_http_response *res = cwist_test_client_get(client, "/ok");
    REQUIRE(res);
    const char *gate = cwist_http_header_get(res->headers, "X-Gate");
    REQUIRE(gate && strcmp(gate, "seen") == 0);
    expect_body(res, 200, "ok");

    /* A middleware that does not call next() keeps the handler from running. */
    expect_body(cwist_test_client_get(client, "/blocked"), 403, NULL);
    REQUIRE(g_mw_calls == 2 && ok.calls == 1 && blocked.calls == 0);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(ok.destroyed == 1 && blocked.destroyed == 1);
    printf("Passed middleware runs around context routes\n");
}

static void test_replacement(void) {
    route_ctx a = {"a", 0, 0}, b = {"b", 0, 0}, c = {"c", 0, 0};
    cwist_app *app = cwist_app_create();
    cwist_test_client *client = cwist_test_client_create(app);

    REQUIRE(registered(cwist_app_get_ex(app, "/x", tag_handler, &a, ctx_destroy)));
    REQUIRE(registered(cwist_app_get_ex(app, "/x", tag_handler, &b, ctx_destroy)));
    REQUIRE(a.destroyed == 1 && b.destroyed == 0);
    expect_body(cwist_test_client_get(client, "/x"), 200, "b");

    /* Registering the same context again keeps it alive. */
    REQUIRE(registered(cwist_app_get_ex(app, "/x", tag_handler, &b, ctx_destroy)));
    REQUIRE(b.destroyed == 0);
    expect_body(cwist_test_client_get(client, "/x"), 200, "b");

    /* A plain handler replacing a context route releases the context. */
    cwist_app_get(app, "/x", plain_handler);
    REQUIRE(b.destroyed == 1);
    expect_body(cwist_test_client_get(client, "/x"), 200, "plain");

    /* ...and a context route can replace a plain one. */
    REQUIRE(registered(cwist_app_get_ex(app, "/x", tag_handler, &c, ctx_destroy)));
    expect_body(cwist_test_client_get(client, "/x"), 200, "c");

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(a.destroyed == 1 && b.destroyed == 1 && c.destroyed == 1);
    REQUIRE(a.calls == 0 && b.calls == 2 && c.calls == 1);
    printf("Passed re-registration releases the replaced context once\n");
}

static void test_null_context(void) {
    g_null_ctx_destroyed = 0;
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_get_ex(app, "/n", null_ctx_handler, NULL, null_ctx_destroy)));
    cwist_test_client *client = cwist_test_client_create(app);
    expect_body(cwist_test_client_get(client, "/n"), 200, "null-ctx");

    /* A NULL context is not "the same context": replacing it still runs the
     * destructor exactly once. */
    REQUIRE(registered(cwist_app_get_ex(app, "/n", null_ctx_handler, NULL, NULL)));
    REQUIRE(g_null_ctx_destroyed == 1);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(g_null_ctx_destroyed == 1);
    printf("Passed NULL context is passed through and released once\n");
}

static void test_failures_release_context(void) {
    route_ctx ctx = {"f", 0, 0};
    cwist_app *app = cwist_app_create();

    REQUIRE(!registered(cwist_app_get_ex(NULL, "/f", tag_handler, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 1);
    REQUIRE(!registered(cwist_app_get_ex(app, NULL, tag_handler, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 2);
    REQUIRE(!registered(cwist_app_post_ex(app, "/f", NULL, &ctx, ctx_destroy)));
    REQUIRE(ctx.destroyed == 3);
    /* Without a destructor the caller keeps the context; nothing to call. */
    REQUIRE(!registered(cwist_app_get_ex(app, NULL, tag_handler, &ctx, NULL)));
    REQUIRE(ctx.destroyed == 3);

    /* The failed registrations left no route behind. */
    cwist_test_client *client = cwist_test_client_create(app);
    expect_body(cwist_test_client_get(client, "/f"), 404, NULL);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(ctx.destroyed == 3 && ctx.calls == 0);
    printf("Passed failed registrations release the context\n");
}

static void test_caller_owned(void) {
    route_ctx ctx = {"mine", 0, 0};
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_get_ex(app, "/mine", tag_handler, &ctx, NULL)));
    cwist_test_client *client = cwist_test_client_create(app);
    expect_body(cwist_test_client_get(client, "/mine"), 200, "mine");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    REQUIRE(ctx.calls == 1 && ctx.destroyed == 0);
    printf("Passed NULL destructor leaves ownership with the caller\n");
}

static void test_multiport_shares_context(void) {
    route_ctx ctx = {"shared", 0, 0};
    cwist_app *root = cwist_app_create();
    root->port = 18080;
    REQUIRE(registered(cwist_app_get_ex(root, "/shared", tag_handler, &ctx, ctx_destroy)));

    cwist_app *sub = cwist_multiport_get_app(&root, 18081);
    REQUIRE(sub && sub != root);
    cwist_test_client *sub_client = cwist_test_client_create(sub);
    expect_body(cwist_test_client_get(sub_client, "/shared"), 200, "shared");
    REQUIRE(ctx.calls == 1);

    /* The sub-app does not own the shared context: replacing the route there
     * must not release it, and the root keeps serving it. */
    cwist_app_get(sub, "/shared", plain_handler);
    REQUIRE(ctx.destroyed == 0);
    expect_body(cwist_test_client_get(sub_client, "/shared"), 200, "plain");
    cwist_test_client_destroy(sub_client);

    cwist_test_client *root_client = cwist_test_client_create(root);
    expect_body(cwist_test_client_get(root_client, "/shared"), 200, "shared");
    cwist_test_client_destroy(root_client);

    cwist_app_destroy(root);
    REQUIRE(ctx.destroyed == 1 && ctx.calls == 2);
    printf("Passed multiport sub-apps share the context without owning it\n");
}

static void test_heap_context(void) {
    /* A heap context released by cwist_free: ASan/LSan catch a leak or a
     * double release here. */
    route_ctx *ctx = (route_ctx *)cwist_alloc(sizeof(*ctx));
    REQUIRE(ctx);
    ctx->tag = "heap";
    ctx->calls = 0;
    ctx->destroyed = 0;
    cwist_app *app = cwist_app_create();
    REQUIRE(registered(cwist_app_get_ex(app, "/heap", tag_handler, ctx, cwist_free)));
    cwist_test_client *client = cwist_test_client_create(app);
    expect_body(cwist_test_client_get(client, "/heap"), 200, "heap");
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed heap context released with the app\n");
}

int main(void) {
    printf("Testing per-route user context...\n");
    test_every_method();
    test_param_route();
    test_middleware();
    test_replacement();
    test_null_context();
    test_failures_release_context();
    test_caller_owned();
    test_multiport_shares_context();
    test_heap_context();
    printf("All route context tests passed!\n");
    return 0;
}
