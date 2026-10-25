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

CWIST_WASM_COMPONENT_DEFINE_GUEST(g_app)

int main(void) {
    g_app = cwist_app_create();
    if (!g_app) return 1;
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_app_get(g_app, "/session/set", session_set_handler);
    cwist_app_get(g_app, "/session/get", session_get_handler);
    return 0;
}
