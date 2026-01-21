#ifndef __CWIST_APP_H__
#define __CWIST_APP_H__

#include <cwist/http.h>
#include <cwist/https.h>
#include <cwist/err/cwist_err.h>

typedef void (*cwist_handler_func)(cwist_http_request *req, cwist_http_response *res);

typedef struct cwist_route_node {
    const char *path;
    cwist_http_method_t method;
    cwist_handler_func handler;
    struct cwist_route_node *next;
} cwist_route_node;

typedef struct cwist_app {
    int port;
    bool use_ssl;
    char *cert_path;
    char *key_path;
    
    // Simple linked list router for now
    cwist_route_node *routes;
    
    // Internal contexts
    cwist_https_context *ssl_ctx;
} cwist_app;

// --- API ---

cwist_app *cwist_app_create(void);
void cwist_app_destroy(cwist_app *app);

cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);

// Routing
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
// Add other methods as needed

// Start
int cwist_app_listen(cwist_app *app, int port);

#endif
