#define _POSIX_C_SOURCE 200809L
#include <cwist/app.h>
#include <cwist/http.h>
#include <cwist/https.h>
#include <cwist/sstring.h>
#include <cwist/json_builder.h> // Helper included for apps, though not strictly used here yet
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
    app->middlewares = NULL;
    app->ssl_ctx = NULL;
    app->error_handler = NULL;
    
    return app;
}

void cwist_app_use(cwist_app *app, cwist_middleware_func mw) {
    if (!app || !mw) return;
    cwist_middleware_node *node = malloc(sizeof(cwist_middleware_node));
    node->func = mw;
    node->next = NULL;

    if (!app->middlewares) {
        app->middlewares = node;
    } else {
        cwist_middleware_node *curr = app->middlewares;
        while (curr->next) curr = curr->next;
        curr->next = node;
    }
}

void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler) {
    if (app) app->error_handler = handler;
}

void cwist_app_destroy(cwist_app *app) {
    if (!app) return;
    if (app->cert_path) free(app->cert_path);
    if (app->key_path) free(app->key_path);
    if (app->ssl_ctx) cwist_https_destroy_context(app->ssl_ctx);
    
    cwist_route_node *curr_r = app->routes;
    while (curr_r) {
        cwist_route_node *next = curr_r->next;
        free(curr_r);
        curr_r = next;
    }

    cwist_middleware_node *curr_m = app->middlewares;
    while (curr_m) {
        cwist_middleware_node *next = curr_m->next;
        free(curr_m);
        curr_m = next;
    }
    
    free(app);
}

// Internal recursion state for middleware
typedef struct {
    cwist_middleware_node *current_mw_node;
    cwist_handler_func final_handler;
} mw_executor_ctx;

static void mw_next_wrapper(cwist_http_request *req, cwist_http_response *res) {
    mw_executor_ctx *ctx = (mw_executor_ctx *)req->private_data;
    if (!ctx) return;

    if (ctx->current_mw_node) {
        cwist_middleware_node *node = ctx->current_mw_node;
        // Advance the chain for the next "next" call
        ctx->current_mw_node = node->next;
        node->func(req, res, mw_next_wrapper);
    } else if (ctx->final_handler) {
        ctx->final_handler(req, res);
    }
}

static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler) {
    mw_executor_ctx ctx = { app->middlewares, final_handler };
    req->private_data = &ctx;
    mw_next_wrapper(req, res);
    req->private_data = NULL;
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

    return cwist_https_init_context(&app->ssl_ctx, cert_path, key_path);
}

static void add_route(cwist_app *app, const char *path, cwist_http_method_t method, cwist_handler_func handler) {
    cwist_route_node *node = (cwist_route_node *)malloc(sizeof(cwist_route_node));
    node->path = path; // Assumes literal or persistent string
    node->method = method;
    node->handler = handler;
    node->ws_handler = NULL;
    node->next = app->routes;
    app->routes = node;
}

void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_GET, handler);
}

void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_POST, handler);
}

void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler) {
    cwist_route_node *node = (cwist_route_node *)malloc(sizeof(cwist_route_node));
    node->path = path;
    node->method = CWIST_HTTP_GET; // Upgrade uses GET
    node->handler = NULL;
    node->ws_handler = handler;
    node->next = app->routes;
    app->routes = node;
}

static bool match_path(const char *pattern, const char *actual, cwist_query_map *params) {
    char p[256], a[256];
    strncpy(p, pattern, 255);
    strncpy(a, actual, 255);
    p[255] = a[255] = '\0';

    char *saveptr_p, *saveptr_a;
    char *tok_p = strtok_r(p, "/", &saveptr_p);
    char *tok_a = strtok_r(a, "/", &saveptr_a);

    while (tok_p && tok_a) {
        if (tok_p[0] == ':') {
            // Path Parameter
            cwist_query_map_set(params, tok_p + 1, tok_a);
        } else if (strcmp(tok_p, tok_a) != 0) {
            return false;
        }
        tok_p = strtok_r(NULL, "/", &saveptr_p);
        tok_a = strtok_r(NULL, "/", &saveptr_a);
    }

    return tok_p == NULL && tok_a == NULL;
}

// Internal Router Logic
static void internal_route_handler(cwist_app *app, cwist_http_request *req, cwist_http_response *res) {
    if (!req) return;
    
    cwist_route_node *curr = app->routes;
    cwist_route_node *found_route = NULL;
    
    while (curr) {
        if (match_path(curr->path, req->path->data, req->path_params)) {
            if (curr->method == req->method) {
                found_route = curr;
                break;
            }
        }
        curr = curr->next;
    }
    
    if (found_route) {
        if (found_route->ws_handler) {
            if (req->client_fd >= 0) {
                cwist_websocket *ws = cwist_websocket_upgrade(req, req->client_fd);
                if (ws) {
                    found_route->ws_handler(ws);
                    cwist_websocket_destroy(ws);
                } else {
                    res->status_code = CWIST_HTTP_BAD_REQUEST;
                    cwist_sstring_assign(res->body, "WebSocket Upgrade Failed");
                }
            }
        } else {
            execute_chain(app, req, res, found_route->handler);
        }
    } else {
        if (app->error_handler) {
            app->error_handler(req, res, CWIST_HTTP_NOT_FOUND);
        } else {
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "404 Not Found");
        }
    }
}

static void static_ssl_handler(cwist_https_connection *conn, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    cwist_http_request *req = cwist_https_receive_request(conn);
    if (!req) return;
    
    cwist_http_response *res = cwist_http_response_create();
    internal_route_handler(app, req, res);
    
    cwist_https_send_response(conn, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void static_http_handler(int client_fd, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    char buffer[8192];
    int bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }
    buffer[bytes] = '\0';
    
    cwist_http_request *req = cwist_http_parse_request(buffer);
    if (!req) {
        close(client_fd);
        return;
    }
    req->client_fd = client_fd;

    cwist_http_response *res = cwist_http_response_create();
    internal_route_handler(app, req, res);
    
    if (!req->upgraded) {
        cwist_http_send_response(client_fd, res);
    }
    
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    close(client_fd);
}

int cwist_app_listen(cwist_app *app, int port) {
    if (!app) return -1;
    app->port = port;
    
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
        cwist_https_server_loop(server_fd, app->ssl_ctx, static_ssl_handler, app);
    } else {
        cwist_server_config config = { .use_forking = false, .use_threading = true, .use_epoll = false };
        cwist_http_server_loop(server_fd, &config, static_http_handler, app);
    }
    
    return 0;
}
