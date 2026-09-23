/* Dispatch guest for the component pipeline (issue #203, stage 2).
 *
 * Same consumer-style app as tests/wasm_wrapper_test.c (routes and handlers
 * kept in sync on purpose), but exporting the cwist-guest world from
 * wit/cwist.wit through wit-bindgen's canonical ABI shims instead of the
 * Emscripten pointer ABI. Compiled for wasm32-wasip2, componentized with
 * wasm-tools, transpiled with jco, and driven from node by
 * tests/wasm_component_test.js via wasm/npm/component.js.
 *
 * Generated bindings (cwist_guest.h / cwist_guest.c) come from
 * `make wit-bindings` and are never committed. */
#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/wasm/wasm_component.h>
#include <string.h>

#include "cwist_guest.h"

static cwist_app *g_app;

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-from-wrapper-test");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "X-Wrapper-Test", "yes");
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) {
        cwist_sstring_assign(res->body, req->body->data);
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
}

static void session_set_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *s = cwist_session_start(g_app, req, res);
    if (!s) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_session_set(s, "user", "alice");
    cwist_session_commit(s, res);
    cwist_sstring_assign(res->body, "session-set");
}

static void session_get_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *s = cwist_session_start(g_app, req, res);
    const char *user = s ? cwist_session_get(s, "user") : NULL;
    cwist_sstring_assign(res->body, user ? user : "anonymous");
}

/* wit-bindgen guest export: fill *ret on success (return true), *err on
 * failure (return false). The canonical ABI copies the returned list out of
 * guest memory and frees it via cabi_realloc, so the response buffer must be
 * a libc allocation. cwist_alloc is exactly that under __wasi__. */
bool exports_c4punks_cwist_guest_dispatch(cwist_guest_list_u8_t *request,
                                          cwist_guest_list_u8_t *ret,
                                          exports_c4punks_cwist_guest_dispatch_error_t *err) {
    uint8_t *res_buf = NULL;
    size_t res_len = 0;
    if (cwist_wasm_component_dispatch(g_app, request->ptr, request->len, &res_buf, &res_len) != 0) {
        err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_DISPATCH_ERROR_INVALID_REQUEST;
        return false;
    }
    ret->ptr = res_buf;
    ret->len = res_len;
    return true;
}

bool exports_c4punks_cwist_guest_use_session(cwist_guest_string_t *maybe_secret,
                                             exports_c4punks_cwist_guest_session_error_t *err) {
    char buf[256];
    const char *secret = NULL;
    if (maybe_secret) {
        if (maybe_secret->len >= sizeof(buf)) {
            err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;
            return false;
        }
        memcpy(buf, maybe_secret->ptr, maybe_secret->len);
        buf[maybe_secret->len] = '\0';
        secret = buf;
    }
    if (cwist_app_use_session(g_app, secret) != 0) {
        err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;
        return false;
    }
    return true;
}

int main(void) {
    g_app = cwist_app_create();
    if (!g_app) return 1;
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_app_get(g_app, "/session/set", session_set_handler);
    cwist_app_get(g_app, "/session/get", session_get_handler);
    return 0;
}
