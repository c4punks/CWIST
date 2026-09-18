/* Integration test for the cwist-wasm JS wrapper (wasm/npm/index.js).
 *
 * Builds a consumer-style app module via include/cwist/wasm/wasm_entry.h,
 * then tests/wasm_wrapper_test.js drives it through the wrapper with no
 * Emscripten-specific code on the JS side. Run:
 *
 *   make wasm-wrapper-test
 *
 * clang-format off regions: EM_JS bodies are JS, not C (see typedarray.h). */
#include <cwist/sys/app/app.h>
#include <cwist/wasm/typedarray.h>
#include <cwist/wasm/wasm_entry.h>
#include <string.h>

static cwist_app *g_app;

CWIST_WASM_INSTALL_VIEWS()

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-from-wrapper-test");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "X-Wrapper-Test", "yes");
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    /* Echo the request body back so the JS side can verify body plumbing. */
    if (req->body && req->body->data) {
        cwist_sstring_assign(res->body, req->body->data);
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
}

/* File scope: the macro defines functions, which C does not allow inside
 * another function body. g_app is populated in main() before any dispatch. */
CWIST_WASM_DEFINE_ENTRY(g_app)

int main(void) {
    g_app = cwist_app_create();
    if (!g_app) return 1;
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_wasm_install_views();
    return 0;
}
