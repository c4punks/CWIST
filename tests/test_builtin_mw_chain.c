/**
 * @file test_builtin_mw_chain.c
 * @brief The JWT and rate-limit middleware inside a real app middleware chain.
 *
 * tests/test_middleware_jwt.c and tests/test_rate_limit.c call the middleware
 * directly with a stand-in next(). Here they run through cwist_app_use() and
 * the in-process test client, so next() is the app's own chain step and the
 * final handler is a real route (plain and cwist_app_get_ex()).
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/security/jwt/jwt.h>
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

static const char *const k_secret = "chain-test-secret";

static int g_after_calls = 0;
static int g_handler_calls = 0;

/* Runs after the built-in middleware, so it is only reached through next(). */
static void after_mw(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    g_after_calls++;
    if (next) next(req, res);
    cwist_http_header_add(&res->headers, "X-After", "yes");
}

/* Replies with the "sub" claim, or "anonymous" without JWT claims. */
static void whoami(cwist_http_request *req, cwist_http_response *res) {
    g_handler_calls++;
    const cwist_jwt_claims *claims = cwist_mw_jwt_get_claims(req);
    const char *sub = claims ? cwist_jwt_claims_get(claims, "sub") : NULL;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, sub ? sub : "anonymous");
}

static void whoami_ex(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    REQUIRE(user_ctx == &g_handler_calls);
    whoami(req, res);
}

static bool registered(cwist_error_t err) {
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

static void expect(cwist_http_response *res, int status, const char *body, bool after) {
    REQUIRE(res != NULL);
    if ((int)res->status_code != status) {
        fprintf(stderr, "FAIL: status %d, want %d\n", (int)res->status_code, status);
        REQUIRE(0);
    }
    if (body) {
        REQUIRE(res->body && res->body->data);
        if (strcmp(res->body->data, body) != 0) {
            fprintf(stderr, "FAIL: body \"%s\", want \"%s\"\n", res->body->data, body);
            REQUIRE(0);
        }
    }
    const char *mark = cwist_http_header_get(res->headers, "X-After");
    REQUIRE(after ? (mark && strcmp(mark, "yes") == 0) : mark == NULL);
    cwist_http_response_destroy(res);
}

static cwist_http_response *get_with_auth(cwist_test_client *client, const char *path,
                                          const char *authorization) {
    cwist_test_client_kv header = {"Authorization", authorization};
    cwist_test_client_request_options opts = {0};
    if (authorization) {
        opts.headers = &header;
        opts.header_count = 1;
    }
    return cwist_test_client_request_ex(client, CWIST_HTTP_GET, path, &opts);
}

static cwist_app *make_app(cwist_middleware_func builtin) {
    cwist_app *app = cwist_app_create();
    REQUIRE(app && builtin);
    cwist_app_use(app, builtin);
    cwist_app_use(app, after_mw);
    cwist_app_get(app, "/plain", whoami);
    REQUIRE(registered(cwist_app_get_ex(app, "/ex", whoami_ex, &g_handler_calls, NULL)));
    return app;
}

static void test_rate_limit_continues_the_chain(void) {
    cwist_mw_rate_limit_reset();
    g_after_calls = g_handler_calls = 0;
    cwist_app *app = make_app(cwist_mw_rate_limit_ip(60));
    cwist_test_client *client = cwist_test_client_create(app);

    /* The test client has no socket, so the limiter cannot resolve a client
     * IP and lets the request through via next(). */
    expect(cwist_test_client_get(client, "/plain"), 200, "anonymous", true);
    expect(cwist_test_client_get(client, "/ex"), 200, "anonymous", true);
    REQUIRE(g_after_calls == 2 && g_handler_calls == 2);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed rate limiter continues the app middleware chain\n");
}

static void test_jwt_continues_the_chain(void) {
    g_after_calls = g_handler_calls = 0;
    cwist_app *app = make_app(cwist_mw_jwt_auth(k_secret));
    cwist_test_client *client = cwist_test_client_create(app);

    char *token = cwist_jwt_sign("{\"sub\":\"alice\"}", k_secret, 3600);
    REQUIRE(token);
    char bearer[1024];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    /* A valid token runs the rest of the chain, and both route kinds see the
     * decoded claims. */
    expect(get_with_auth(client, "/plain", bearer), 200, "alice", true);
    expect(get_with_auth(client, "/ex", bearer), 200, "alice", true);
    REQUIRE(g_after_calls == 2 && g_handler_calls == 2);

    /* Rejected requests short-circuit before the rest of the chain. */
    expect(get_with_auth(client, "/ex", NULL), 401, NULL, false);
    expect(get_with_auth(client, "/ex", "Bearer not-a-token"), 401, NULL, false);
    REQUIRE(g_after_calls == 2 && g_handler_calls == 2);

    cwist_free(token);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed JWT middleware continues the app middleware chain\n");
}

static void test_jwt_behind_rate_limit(void) {
    cwist_mw_rate_limit_reset();
    g_after_calls = g_handler_calls = 0;
    cwist_app *app = cwist_app_create();
    REQUIRE(app);
    cwist_app_use(app, cwist_mw_rate_limit_ip(60));
    cwist_app_use(app, cwist_mw_jwt_auth(k_secret));
    cwist_app_use(app, after_mw);
    REQUIRE(registered(cwist_app_get_ex(app, "/ex", whoami_ex, &g_handler_calls, NULL)));
    cwist_test_client *client = cwist_test_client_create(app);

    char *token = cwist_jwt_sign("{\"sub\":\"bob\"}", k_secret, 3600);
    REQUIRE(token);
    char bearer[1024];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    expect(get_with_auth(client, "/ex", bearer), 200, "bob", true);
    REQUIRE(g_after_calls == 1 && g_handler_calls == 1);

    cwist_free(token);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("Passed JWT middleware behind the rate limiter\n");
}

static bool g_saw_claims = false;
static void claims_probe(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    g_saw_claims = cwist_mw_jwt_get_claims(req) != NULL;
}

static void test_claims_scope(void) {
    /* Claims are visible only to the request being authenticated, and only
     * until the middleware's next() returns. */
    cwist_middleware_func mw = cwist_mw_jwt_auth(k_secret);
    REQUIRE(mw);
    char *token = cwist_jwt_sign("{\"sub\":\"carol\"}", k_secret, 3600);
    REQUIRE(token);
    char bearer[1024];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_request *other = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    REQUIRE(req && other && res);
    cwist_http_header_add(&req->headers, "Authorization", bearer);

    g_saw_claims = false;
    mw(req, res, claims_probe);
    REQUIRE(g_saw_claims);
    REQUIRE(cwist_mw_jwt_get_claims(req) == NULL);
    REQUIRE(cwist_mw_jwt_get_claims(other) == NULL);
    REQUIRE(cwist_mw_jwt_get_claims(NULL) == NULL);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(other);
    cwist_http_request_destroy(req);
    cwist_free(token);
    printf("Passed JWT claims are scoped to the request and the chain\n");
}

int main(void) {
    printf("Testing built-in middleware inside the app chain...\n");
    test_rate_limit_continues_the_chain();
    test_jwt_continues_the_chain();
    test_jwt_behind_rate_limit();
    test_claims_scope();
    printf("All built-in middleware chain tests passed.\n");
    return 0;
}
