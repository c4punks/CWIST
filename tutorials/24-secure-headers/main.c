#include <cwist/app.h>

static void handle_secure_page(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_add_security_headers(res);
    cwist_sstring_assign(res->body, "<html><body><h1>Hardened Security Headers</h1></body></html>");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_secure_page);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
