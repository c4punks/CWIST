/**
 * @file main.c
 * @brief 18-https-hello — minimal HTTPS server (TLS backend smoke target).
 *
 * Used by the fat-archive CI jobs to prove an app linked against the
 * fat-dist bundle serves HTTPS; with CWIST_TLS=mbedtls the link pulls in no
 * BoringSSL objects at all.  Expects server.crt / server.key next to the
 * binary (generate with e.g. openssl req -x509 -newkey rsa:2048 -nodes).
 */

#include <cwist/app.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello, CWIST!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_https(app, "server.crt", "server.key");
    cwist_app_get(app, "/", hello);
    cwist_app_listen(app, 8443);
    cwist_app_destroy(app);
    return 0;
}
