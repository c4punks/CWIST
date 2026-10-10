/**
 * @file rss_gate_server.c
 * @brief Minimal HTTPS server driven by scripts/ci/rss_gate.sh.
 *
 * Serves GET / ("ok") and GET /big (64 KiB) over TLS with the regular
 * cwist_app_listen() path, so the gate measures the resident memory a real
 * deployment pays: worker processes, the HTTPS pool, park set and reactors.
 * Not a unit test (it runs until SIGTERM), so it is not in TEST_TARGETS.
 */
#include <cwist/sys/app/app.h>
#include <stdlib.h>
#include <string.h>

static char g_big[64 * 1024];

static void ok_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "ok");
}

static void big_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
    cwist_sstring_assign_len(res->body, g_big, sizeof(g_big));
}

int main(int argc, char **argv) {
    const char *cert = argc > 2 ? argv[2] : "example/othello-web/server.crt";
    const char *key = argc > 3 ? argv[3] : "example/othello-web/server.key";
    memset(g_big, 'x', sizeof(g_big));
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_error_t err = cwist_app_use_https(app, cert, key);
    if (!cwist_error_is_ok(&err)) return 1;
    cwist_app_get(app, "/", ok_route);
    cwist_app_get(app, "/big", big_route);
    int rc = cwist_app_listen(app, argc > 1 ? atoi(argv[1]) : 19480);
    cwist_app_destroy(app);
    return rc;
}
