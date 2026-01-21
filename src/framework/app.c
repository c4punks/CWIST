#define _POSIX_C_SOURCE 200809L
#include <cwist/app.h>
#include <cwist/http.h>
#include <cwist/https.h>
#include <cwist/sstring.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

cwist_app *cwist_app_create(void) {
    cwist_app *app = (cwist_app *)malloc(sizeof(cwist_app));
    if (!app) return NULL;
    
    app->port = 8080;
    app->use_ssl = false;
    app->cert_path = NULL;
    app->key_path = NULL;
    app->routes = NULL;
    app->ssl_ctx = NULL;
    
    return app;
}

void cwist_app_destroy(cwist_app *app) {
    if (!app) return;
    if (app->cert_path) free(app->cert_path);
    if (app->key_path) free(app->key_path);
    if (app->ssl_ctx) cwist_https_destroy_context(app->ssl_ctx);
    
    cwist_route_node *curr = app->routes;
    while (curr) {
        cwist_route_node *next = curr->next;
        free(curr); // Path is usually string literal, but if copied, free here.
        curr = next;
    }
    
    free(app);
}

cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }
    
    app->use_ssl = true;
    app->cert_path = strdup(cert_path);
    app->key_path = strdup(key_path);
    
    // Init Context immediately to fail fast
    return cwist_https_init_context(&app->ssl_ctx, cert_path, key_path);
}

static void add_route(cwist_app *app, const char *path, cwist_http_method_t method, cwist_handler_func handler) {
    cwist_route_node *node = (cwist_route_node *)malloc(sizeof(cwist_route_node));
    node->path = path; // Assumes literal or persistent string
    node->method = method;
    node->handler = handler;
    node->next = app->routes;
    app->routes = node;
}

void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_GET, handler);
}

void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_POST, handler);
}

// Internal Router Logic
static void internal_route_handler(cwist_app *app, cwist_http_request *req, cwist_http_response *res) {
    if (!req) return;
    
    cwist_route_node *curr = app->routes;
    bool found = false;
    
    while (curr) {
        // Simple exact match for now. TODO: Regex/Parameter matching
        if (strcmp(curr->path, req->path->data) == 0 && curr->method == req->method) {
            curr->handler(req, res);
            found = true;
            break;
        }
        curr = curr->next;
    }
    
    if (!found) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "404 Not Found");
    }
}

// Global reference for the static handler wrapper (C limitation without closures)
static cwist_app *GLOBAL_APP_REF = NULL;

static void static_ssl_handler(cwist_https_connection *conn) {
    cwist_http_request *req = cwist_https_receive_request(conn);
    if (!req) return;
    
    cwist_http_response *res = cwist_http_response_create();
    
    internal_route_handler(GLOBAL_APP_REF, req, res);
    
    cwist_https_send_response(conn, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void static_http_handler(int client_fd) {
    // We need to read raw from fd since we don't have SSL wrapper
    // But cwist_http_parse_request expects a string.
    char buffer[8192];
    int bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }
    buffer[bytes] = '\0';
    
    cwist_http_request *req = cwist_http_parse_request(buffer);
    cwist_http_response *res = cwist_http_response_create();
    
    internal_route_handler(GLOBAL_APP_REF, req, res);
    
    cwist_http_send_response(client_fd, res);
    
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    close(client_fd);
}

int cwist_app_listen(cwist_app *app, int port) {
    if (!app) return -1;
    app->port = port;
    GLOBAL_APP_REF = app; // Bind for static handlers
    
    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", port, 128);
    if (server_fd < 0) {
        perror("Failed to bind port");
        return -1;
    }
    
    printf("CWIST App running on port %d (SSL: %s)\n", port, app->use_ssl ? "On" : "Off");
    
    if (app->use_ssl) {
        if (!app->ssl_ctx) {
            fprintf(stderr, "SSL enabled but context not initialized.\n");
            return -1;
        }
        cwist_https_server_loop(server_fd, app->ssl_ctx, static_ssl_handler);
    } else {
        cwist_server_config config = { .use_forking = false, .use_threading = true, .use_epoll = false };
        cwist_http_server_loop(server_fd, &config, static_http_handler);
    }
    
    return 0;
}
