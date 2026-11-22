#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/cookie.h>
#include <cwist/net/http/async.h>
#include <cwist/core/mem/gc.h>
#include "../src/net/http/async_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static int header_contains(cwist_http_header_node *headers, const char *key, const char *value) {
    cwist_http_header_node *curr = headers;
    while (curr) {
        if (curr->key && curr->value && strcasecmp(curr->key->data, key) == 0 &&
            strstr(curr->value->data, value) != NULL) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

static void session_cookie_value(cwist_http_response *res, char *out, size_t out_size) {
    const char *set_cookie = cwist_http_header_get(res->headers, "Set-Cookie");
    assert(set_cookie != NULL);
    const char *eq = strchr(set_cookie, '=');
    const char *semi = eq ? strchr(eq, ';') : NULL;
    assert(eq != NULL);
    size_t len = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
    assert(len < out_size);
    memcpy(out, eq + 1, len);
    out[len] = '\0';
}

static void session_set_cookie(cwist_http_request *req, const char *value) {
    char header[8192];
    snprintf(header, sizeof(header), "cwist_session=%s", value);
    cwist_http_header_add(&req->headers, "Cookie", header);
}

static void test_session_expiry_and_revocation(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "expiry-secret") == 0);
    cwist_app_set_session_max_age(app, 1);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);
    char revoked[4096];
    session_cookie_value(res, revoked, sizeof(revoked));
    cwist_session_invalidate(session);
    assert(cwist_session_commit(session, res) == 0);

    cwist_http_request *replayed = cwist_http_request_create();
    cwist_http_response *replay_res = cwist_http_response_create();
    session_set_cookie(replayed, revoked);
    cwist_session_t *replayed_session = cwist_session_start(app, replayed, replay_res);
    assert(replayed_session != NULL);
    assert(cwist_session_get(replayed_session, "role") == NULL);
    cwist_session_destroy(replayed_session);
    replayed->session = NULL;
    cwist_http_response_destroy(replay_res);
    cwist_http_request_destroy(replayed);
    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    char decoded[4096];
    assert(cwist_cookie_decode(revoked, decoded, sizeof(decoded)) >= 0);
    size_t decoded_len = strlen(decoded);
    assert(decoded_len > 2 && decoded[decoded_len - 1] == '=');
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *last = strchr(alphabet, decoded[decoded_len - 2]);
    assert(last != NULL);
    size_t index = (size_t)(last - alphabet);
    decoded[decoded_len - 2] = alphabet[(index & 0x3c) | ((index + 1) & 3)];
    char *noncanonical = cwist_cookie_encode(decoded);
    assert(noncanonical != NULL);
    replayed = cwist_http_request_create();
    replay_res = cwist_http_response_create();
    session_set_cookie(replayed, noncanonical);
    replayed_session = cwist_session_start(app, replayed, replay_res);
    assert(cwist_session_get(replayed_session, "role") == NULL);
    cwist_session_destroy(replayed_session);
    replayed->session = NULL;
    free(noncanonical);
    cwist_http_response_destroy(replay_res);
    cwist_http_request_destroy(replayed);

    req = cwist_http_request_create();
    res = cwist_http_response_create();
    session = cwist_session_start(app, req, res);
    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);
    char expired[4096];
    session_cookie_value(res, expired, sizeof(expired));
    assert(strcmp(revoked, expired) != 0);
    sleep(2);

    replayed = cwist_http_request_create();
    replay_res = cwist_http_response_create();
    session_set_cookie(replayed, expired);
    replayed_session = cwist_session_start(app, replayed, replay_res);
    assert(cwist_session_get(replayed_session, "role") == NULL);
    cwist_session_destroy(replayed_session);
    replayed->session = NULL;
    cwist_http_response_destroy(replay_res);
    cwist_http_request_destroy(replayed);
    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    puts("[session] expiry and revocation OK");
}

static void test_session_create_and_commit(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "my-super-secret-key") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    assert(req != NULL && res != NULL);

    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_set(session, "user_id", "42") == 0);
    assert(cwist_session_commit(session, res) == 0);

    assert(header_contains(res->headers, "Set-Cookie", "cwist_session="));
    assert(header_contains(res->headers, "Set-Cookie", "HttpOnly"));
    assert(!header_contains(res->headers, "Set-Cookie", "Secure"));

    cwist_session_destroy(session);
    req->session = NULL;
    app->use_ssl = true;
    cwist_http_response *secure_res = cwist_http_response_create();
    cwist_session_t *secure_session = cwist_session_start(app, req, secure_res);
    assert(secure_session != NULL);
    assert(cwist_session_set(secure_session, "user_id", "42") == 0);
    assert(cwist_session_commit(secure_session, secure_res) == 0);
    assert(header_contains(secure_res->headers, "Set-Cookie", "Secure"));
    cwist_http_response_destroy(secure_res);

    /* Delete removes one key; invalidate clears the cookie on the next commit. */
    assert(cwist_session_set(secure_session, "to_delete", "val") == 0);
    cwist_session_delete(secure_session, "to_delete");
    assert(cwist_session_get(secure_session, "to_delete") == NULL);
    assert(strcmp(cwist_session_get(secure_session, "user_id"), "42") == 0);
    cwist_session_invalidate(secure_session);
    cwist_http_response *cleared_res = cwist_http_response_create();
    assert(cleared_res != NULL);
    assert(cwist_session_commit(secure_session, cleared_res) == 0);
    assert(header_contains(cleared_res->headers, "Set-Cookie", "cwist_session=;"));
    assert(header_contains(cleared_res->headers, "Set-Cookie", "Max-Age=0"));
    cwist_http_response_destroy(cleared_res);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[session] create and commit OK\n");
}

static void test_session_round_trip(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "another-secret") == 0);
    cwist_app_set_session_name(app, "session2");

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);

    const char *cookie_value = NULL;
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key && strcasecmp(curr->key->data, "Set-Cookie") == 0) {
            cookie_value = curr->value->data;
            break;
        }
        curr = curr->next;
    }
    assert(cookie_value != NULL);

    const char *eq = strchr(cookie_value, '=');
    assert(eq != NULL);
    const char *semi = strchr(eq, ';');
    char value[4096];
    size_t len = semi ? (size_t)(semi - (eq + 1)) : strlen(eq + 1);
    assert(len < sizeof(value));
    memcpy(value, eq + 1, len);
    value[len] = '\0';

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    cwist_http_request *req2 = cwist_http_request_create();
    cwist_http_response *res2 = cwist_http_response_create();
    char cookie_header[8192];
    snprintf(cookie_header, sizeof(cookie_header), "session2=%s", value);
    cwist_http_header_add(&req2->headers, "Cookie", cookie_header);

    cwist_session_t *session2 = cwist_session_start(app, req2, res2);
    assert(session2 != NULL);
    const char *role = cwist_session_get(session2, "role");
    assert(role != NULL && strcmp(role, "admin") == 0);

    cwist_http_response_destroy(res2);
    cwist_http_request_destroy(req2);
    cwist_app_destroy(app);
    printf("[session] round trip OK\n");
}

static void test_session_large_round_trip(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "large-session-secret") == 0);

    size_t len = 5000;
    char *large = malloc(len + 1);
    assert(large != NULL);
    for (size_t i = 0; i < len; i++) large[i] = (char)('a' + i % 26);
    large[len] = '\0';

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_set(session, "large", large) == 0);
    assert(cwist_session_commit(session, res) == 0);

    const char *set_cookie = cwist_http_header_get(res->headers, "Set-Cookie");
    assert(set_cookie != NULL);
    const char *eq = strchr(set_cookie, '=');
    const char *semi = eq ? strchr(eq, ';') : NULL;
    assert(eq && semi && (size_t)(semi - eq) > len);
    size_t cookie_len = (size_t)(semi - eq - 1);
    char *cookie_header = malloc(cookie_len + 32);
    assert(cookie_header != NULL);
    snprintf(cookie_header, cookie_len + 32, "cwist_session=%.*s", (int)cookie_len, eq + 1);

    cwist_http_request *req2 = cwist_http_request_create();
    cwist_http_response *res2 = cwist_http_response_create();
    cwist_http_header_add(&req2->headers, "Cookie", cookie_header);
    cwist_session_t *session2 = cwist_session_start(app, req2, res2);
    assert(session2 != NULL);
    const char *loaded = cwist_session_get(session2, "large");
    assert(loaded != NULL && strlen(loaded) == len && memcmp(loaded, large, len) == 0);

    free(cookie_header);
    free(large);
    cwist_http_response_destroy(res2);
    cwist_http_request_destroy(req2);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[session] large round trip OK\n");
}

static void test_session_invalid_signature(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "secret-a") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_http_header_add(&req->headers, "Cookie", "cwist_session=ZW1wdHk.invalidsig");

    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(cwist_session_get(session, "x") == NULL);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[session] invalid signature rejected OK\n");
}

static void test_session_repeated_commit_revokes_every_cookie(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "stable-jti-secret") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);
    char first[4096];
    session_cookie_value(res, first, sizeof(first));

    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);
    char second[4096];
    session_cookie_value(res, second, sizeof(second));

    cwist_http_response *idle_res = cwist_http_response_create();
    assert(cwist_session_commit(session, idle_res) == 0);
    assert(cwist_http_header_get(idle_res->headers, "Set-Cookie") == NULL);
    cwist_http_response_destroy(idle_res);

    cwist_session_invalidate(session);
    assert(cwist_session_commit(session, res) == 0);
    const char *cookies[] = {first, second};
    for (size_t i = 0; i < 2; i++) {
        cwist_http_request *replayed = cwist_http_request_create();
        cwist_http_response *replay_res = cwist_http_response_create();
        session_set_cookie(replayed, cookies[i]);
        cwist_session_t *loaded = cwist_session_start(app, replayed, replay_res);
        assert(loaded != NULL);
        assert(cwist_session_get(loaded, "role") == NULL);
        cwist_session_destroy(loaded);
        replayed->session = NULL;
        cwist_http_response_destroy(replay_res);
        cwist_http_request_destroy(replayed);
    }

    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    puts("[session] repeated commits revoke every cookie OK");
}

static void test_session_secret_reconfiguration(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "initial-secret-12345") == 0);
    assert(app->session_secret != NULL);
    assert(strcmp(app->session_secret, "initial-secret-12345") == 0);

    /* Self-aliasing reconfiguration with exact pointer (Issue #767) */
    assert(cwist_app_use_session(app, app->session_secret) == 0);
    assert(app->session_secret != NULL);
    assert(strcmp(app->session_secret, "initial-secret-12345") == 0);

    /* Self-aliasing reconfiguration with interior pointer */
    assert(cwist_app_use_session(app, app->session_secret + 8) == 0);
    assert(app->session_secret != NULL);
    assert(strcmp(app->session_secret, "secret-12345") == 0);

    /* Reconfiguration with a new distinct secret */
    assert(cwist_app_use_session(app, "replacement-secret") == 0);
    assert(strcmp(app->session_secret, "replacement-secret") == 0);

    /* Reconfiguration generating a random secret */
    assert(cwist_app_use_session(app, NULL) == 0);
    assert(app->session_secret != NULL);
    assert(strlen(app->session_secret) > 0);

    cwist_app_destroy(app);
    printf("[session] secret reconfiguration OK\n");
}

static void deferred_next(cwist_http_request *req, cwist_http_response *res) {
    assert(cwist_session_set(req->session, "seen", "1") == 0);
    cwist_async_defer(req, res);
}

static char deferred_cookie[4096];

static void capture_deferred_cookie(cwist_async *a, void *ctx) {
    (void)ctx;
    cwist_http_response *res = cwist_async_final_response(a);
    const char *cookie = cwist_http_header_get(res->headers, "Set-Cookie");
    snprintf(deferred_cookie, sizeof(deferred_cookie), "%s", cookie ? cookie : "");
}

static void test_session_deferred_commit(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "deferred-session-secret") == 0);
    cwist_middleware_func mw = cwist_mw_session(app);
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->app = app;
    cwist_full_gc(true);
    mw(req, res, deferred_next);
    cwist_async *a = res->async;
    assert(a != NULL);
    assert(cwist_async_add_completion_hook(a, capture_deferred_cookie, NULL));
    cwist_async_dispatch_ack(a);
    cwist_http_response *final_res = cwist_http_response_create();
    assert(final_res != NULL);
    assert(cwist_async_respond_with(a, final_res));
    cwist_full_gc(false);
    assert(deferred_cookie[0] != '\0');
    cwist_app_destroy(app);
}

/* #958: an untouched session writes no cookie. #957: invalidating a
 * __Host- session deletes it with the attributes it was issued with. */
static void test_session_untouched_and_prefixed_logout(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "untouched-secret") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_get(session, "anything") == NULL);
    assert(cwist_session_commit(session, res) == 0);
    assert(cwist_http_header_get(res->headers, "Set-Cookie") == NULL);
    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    app->use_ssl = true;
    cwist_app_set_session_name(app, "__Host-session");
    req = cwist_http_request_create();
    res = cwist_http_response_create();
    session = cwist_session_start(app, req, res);
    assert(cwist_session_set(session, "user", "alice") == 0);
    assert(cwist_session_commit(session, res) == 0);
    assert(header_contains(res->headers, "Set-Cookie", "__Host-session="));
    char issued[4096];
    session_cookie_value(res, issued, sizeof(issued));
    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    req = cwist_http_request_create();
    res = cwist_http_response_create();
    char header[8192];
    snprintf(header, sizeof(header), "__Host-session=%s", issued);
    cwist_http_header_add(&req->headers, "Cookie", header);
    session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_get(session, "user") != NULL);
    cwist_session_invalidate(session);
    assert(cwist_session_commit(session, res) == 0);
    const char *cleared = cwist_http_header_get(res->headers, "Set-Cookie");
    assert(cleared != NULL);
    assert(strncmp(cleared, "__Host-session=;", 16) == 0);
    assert(strstr(cleared, "Max-Age=0") && strstr(cleared, "Path=/") &&
           strstr(cleared, "Secure") && strstr(cleared, "HttpOnly"));
    cwist_session_destroy(session);
    req->session = NULL;
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    app->use_ssl = false;
    cwist_app_destroy(app);
    printf("[session] untouched session and __Host- logout OK\n");
}

int main(void) {
    test_session_untouched_and_prefixed_logout();
    test_session_deferred_commit();
    test_session_repeated_commit_revokes_every_cookie();
    test_session_create_and_commit();
    test_session_round_trip();
    test_session_large_round_trip();
    test_session_invalid_signature();
    test_session_expiry_and_revocation();
    test_session_secret_reconfiguration();
    printf("All session tests passed.\n");
    return 0;
}
