#include "app.h"

#include <cwist/app.h>
#include <cwist/sys/port/lwip.h>

static cwist_app *g_app;
static cwist_lwip_server *g_server;

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello from stm32h743", 20);
    cwist_http_response_header_add(res, "Content-Type", "text/plain");
}

static void json_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    static const char body[] = "{\"board\":\"nucleo-h743zi2\",\"rtos\":false}";
    cwist_http_response_set_body_ptr(res, body, sizeof(body) - 1);
    cwist_http_response_header_add(res, "Content-Type", "application/json");
}

int cwist_stm32_app_init(void) {
    if (g_server) return 0;
    g_app = cwist_app_create();
    if (!g_app) return -1;
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_get(g_app, "/json", json_handler);

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_conns = 4;
    cfg.max_header_bytes = 4096;
    cfg.max_body_bytes = 1024;
    cfg.idle_timeout_ms = 30000;
    g_server = cwist_lwip_listen(g_app, &cfg);
    if (!g_server) {
        cwist_app_destroy(g_app);
        g_app = NULL;
        return -1;
    }
    return 0;
}
