#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/config.h>
#include <cwist/sys/app/logger.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/health/healthz.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/http/http3.h>
#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/utils/json_builder.h> // Helper included for apps, though not strictly used here yet
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>

#define CWIST_ROUTE_BUCKETS 127

typedef struct cwist_route_entry {
    char *path;
    bool has_params;
    cwist_http_method_t method;
    cwist_handler_func handler;
    cwist_ws_handler_func ws_handler;
    struct cwist_route_entry *next;
} cwist_route_entry;

struct cwist_route_table {
    size_t bucket_count;
    cwist_route_entry **buckets;
    cwist_route_entry *param_routes;
};

struct cwist_static_dir {
    char *url_prefix;
    char *fs_root;
    struct cwist_static_dir *next;
};

typedef struct {
    cwist_middleware_node *current_mw_node;
    cwist_handler_func final_handler;
    void *handler_data;
} mw_executor_ctx;

typedef struct {
    cwist_static_dir *mapping;
    const char *relative_ptr;
    bool use_index;
} cwist_static_request_info;

static cwist_route_table *cwist_route_table_create(void);
static void cwist_route_table_destroy(cwist_route_table *table);
static void cwist_route_table_insert(cwist_route_table *table, const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler);
static cwist_route_entry *cwist_route_table_lookup(cwist_route_table *table, cwist_http_method_t method, const char *path);
static cwist_route_entry *cwist_route_table_match_params(cwist_route_table *table, cwist_http_request *req);
static bool match_path(const char *pattern, const char *actual, cwist_query_map *params);
static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data);
static bool cwist_prepare_static(cwist_app *app, cwist_http_request *req, cwist_static_request_info *info);
static void cwist_static_handler(cwist_http_request *req, cwist_http_response *res);
static void cwist_multiport_destroy_owned_subapps(cwist_app *root);
static void cwist_multiport_unlink_app(cwist_app *app);

static bool route_has_params(const char *path) {
    if (!path) return false;
    return strchr(path, ':') != NULL;
}

static size_t cwist_route_hash(cwist_http_method_t method, const char *path, size_t bucket_count) {
    const unsigned long long FNV_OFFSET = 1469598103934665603ULL;
    const unsigned long long FNV_PRIME = 1099511628211ULL;
    unsigned long long hash = FNV_OFFSET ^ (unsigned long long)method;
    const unsigned char *ptr = (const unsigned char *)path;
    while (ptr && *ptr) {
        hash ^= (unsigned long long)(*ptr++);
        hash *= FNV_PRIME;
    }
    return (size_t)(hash % bucket_count);
}

static cwist_route_entry *cwist_route_entry_create(const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler) {
    cwist_route_entry *entry = (cwist_route_entry *)malloc(sizeof(cwist_route_entry));
    if (!entry) return NULL;
    entry->path = strdup(path ? path : "/");
    entry->method = method;
    entry->handler = handler;
    entry->ws_handler = ws_handler;
    entry->has_params = route_has_params(entry->path);
    entry->next = NULL;
    return entry;
}

static void cwist_route_entry_free(cwist_route_entry *entry) {
    if (!entry) return;
    free(entry->path);
    free(entry);
}

static cwist_route_table *cwist_route_table_create(void) {
    cwist_route_table *table = (cwist_route_table *)malloc(sizeof(cwist_route_table));
    if (!table) return NULL;
    table->bucket_count = CWIST_ROUTE_BUCKETS;
    table->buckets = (cwist_route_entry **)calloc(table->bucket_count, sizeof(cwist_route_entry *));
    if (!table->buckets) {
        free(table);
        return NULL;
    }
    table->param_routes = NULL;
    return table;
}

static void cwist_route_table_destroy(cwist_route_table *table) {
    if (!table) return;
    for (size_t i = 0; i < table->bucket_count; i++) {
        cwist_route_entry *curr = table->buckets[i];
        while (curr) {
            cwist_route_entry *next = curr->next;
            cwist_route_entry_free(curr);
            curr = next;
        }
    }
    free(table->buckets);

    cwist_route_entry *param = table->param_routes;
    while (param) {
        cwist_route_entry *next = param->next;
        cwist_route_entry_free(param);
        param = next;
    }
    free(table);
}

static void cwist_route_table_insert(cwist_route_table *table, const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler) {
    if (!table || !path) return;
    cwist_route_entry *entry = cwist_route_entry_create(path, method, handler, ws_handler);
    if (!entry) return;

    if (entry->has_params) {
        entry->next = table->param_routes;
        table->param_routes = entry;
        return;
    }

    size_t idx = cwist_route_hash(method, entry->path, table->bucket_count);
    cwist_route_entry **bucket = &table->buckets[idx];
    cwist_route_entry *curr = *bucket;
    while (curr) {
        if (!curr->has_params && curr->method == method && strcmp(curr->path, entry->path) == 0) {
            curr->handler = handler;
            curr->ws_handler = ws_handler;
            cwist_route_entry_free(entry);
            return;
        }
        curr = curr->next;
    }

    entry->next = *bucket;
    *bucket = entry;
}

static cwist_route_entry *cwist_route_table_lookup(cwist_route_table *table, cwist_http_method_t method, const char *path) {
    if (!table || !path) return NULL;
    size_t idx = cwist_route_hash(method, path, table->bucket_count);
    cwist_route_entry *curr = table->buckets[idx];
    while (curr) {
        if (curr->method == method && strcmp(curr->path, path) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static cwist_route_entry *cwist_route_table_match_params(cwist_route_table *table, cwist_http_request *req) {
    if (!table || !req || !req->path || !req->path->data) return NULL;
    cwist_route_entry *curr = table->param_routes;
    while (curr) {
        if (curr->method == req->method) {
            if (match_path(curr->path, req->path->data, req->path_params)) {
                return curr;
            }
        }
        curr = curr->next;
    }
    return NULL;
}

static bool cwist_path_has_parent_ref(const char *path) {
    if (!path) return false;
    const char *cursor = path;
    while (*cursor) {
        if (*cursor == '.') {
            char prev = (cursor == path) ? '/' : *(cursor - 1);
            char next = *(cursor + 1);
            char next_next = *(cursor + 2);
            if (prev == '/' && next == '.' && (next_next == '/' || next_next == '\0')) {
                return true;
            }
        }
        cursor++;
    }
    return false;
}

static bool cwist_static_match_entry(const cwist_static_dir *entry, const char *req_path, const char **relative_ptr, bool *use_index) {
    if (!entry || !req_path || req_path[0] == '\0') return false;
    size_t prefix_len = strlen(entry->url_prefix);
    if (prefix_len == 0) return false;
    if (prefix_len == 1 && entry->url_prefix[0] == '/') {
        if (req_path[0] != '/') return false;
        if (req_path[1] == '\0') {
            if (use_index) *use_index = true;
            if (relative_ptr) *relative_ptr = NULL;
        } else {
            if (use_index) *use_index = false;
            if (relative_ptr) *relative_ptr = req_path + 1;
        }
        return true;
    }

    if (strncmp(req_path, entry->url_prefix, prefix_len) != 0) {
        return false;
    }

    char separator = req_path[prefix_len];
    if (separator == '\0') {
        if (use_index) *use_index = true;
        if (relative_ptr) *relative_ptr = NULL;
        return true;
    }
    if (separator != '/') {
        return false;
    }
    if (use_index) *use_index = false;
    if (relative_ptr) *relative_ptr = req_path + prefix_len + 1;
    return true;
}

static bool cwist_prepare_static(cwist_app *app, cwist_http_request *req, cwist_static_request_info *info) {
    if (!app || !req || !req->path || !req->path->data) return false;
    if (!app->static_dirs) return false;
    if (req->method != CWIST_HTTP_GET && req->method != CWIST_HTTP_HEAD) return false;

    cwist_static_dir *entry = app->static_dirs;
    const char *path = req->path->data;
    while (entry) {
        bool use_index = false;
        const char *relative = NULL;
        if (cwist_static_match_entry(entry, path, &relative, &use_index)) {
            if (info) {
                info->mapping = entry;
                info->relative_ptr = relative;
                info->use_index = use_index;
            }
            return true;
        }
        entry = entry->next;
    }
    return false;
}

static char *cwist_normalize_prefix(const char *prefix) {
    if (!prefix || prefix[0] == '\0') {
        return strdup("/");
    }

    size_t len = strlen(prefix);
    bool needs_leading_slash = prefix[0] != '/';
    char *buffer = (char *)malloc(len + needs_leading_slash + 1);
    if (!buffer) {
        return NULL;
    }

    if (needs_leading_slash) {
        buffer[0] = '/';
        memcpy(buffer + 1, prefix, len + 1);
        len += 1;
    } else {
        memcpy(buffer, prefix, len + 1);
    }

    while (len > 1 && buffer[len - 1] == '/') {
        buffer[len - 1] = '\0';
        len--;
    }

    return buffer;
}

static char *cwist_normalize_directory(const char *directory) {
    if (!directory || directory[0] == '\0') {
        return strdup(".");
    }
    size_t len = strlen(directory);
    while (len > 1 && directory[len - 1] == '/') {
        len--;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, directory, len);
    copy[len] = '\0';
    return copy;
}

static void cwist_static_handler(cwist_http_request *req, cwist_http_response *res) {
    mw_executor_ctx *ctx = (mw_executor_ctx *)req->private_data;
    cwist_static_request_info *info = ctx ? (cwist_static_request_info *)ctx->handler_data : NULL;
    if (!info || !info->mapping) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Static handler misconfigured");
        return;
    }

    char relative_buf[PATH_MAX];
    if (info->use_index || !info->relative_ptr || info->relative_ptr[0] == '\0') {
        snprintf(relative_buf, sizeof(relative_buf), "index.html");
    } else {
        snprintf(relative_buf, sizeof(relative_buf), "%s", info->relative_ptr);
    }
    relative_buf[PATH_MAX - 1] = '\0';

    if (cwist_path_has_parent_ref(relative_buf)) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Directory traversal blocked");
        return;
    }

    char fs_path[PATH_MAX];
    int written = snprintf(fs_path, sizeof(fs_path), "%s/%s", info->mapping->fs_root, relative_buf);
    if (written < 0 || written >= (int)sizeof(fs_path)) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Static path too long");
        return;
    }

    cwist_app *app = req->app;
    if (!app || !app->mem_manager) {
         res->status_code = CWIST_HTTP_INTERNAL_ERROR;
         cwist_sstring_assign(res->body, "Server memory not initialized");
         return;
    }

    cwist_fix_server_mem *mem = app->mem_manager;
    pthread_mutex_lock(&mem->lock);
    cwist_file_t *file = cwist_mem_get_file(mem, fs_path);
    
    if (file) {
        // Simple MIME guess
        const char *dot = strrchr(fs_path, '.');
        const char *mime = "application/octet-stream";
        if (dot) {
            if (strcasecmp(dot, ".html") == 0) mime = "text/html; charset=utf-8";
            else if (strcasecmp(dot, ".css") == 0) mime = "text/css; charset=utf-8";
            else if (strcasecmp(dot, ".js") == 0) mime = "application/javascript";
            else if (strcasecmp(dot, ".json") == 0) mime = "application/json";
            else if (strcasecmp(dot, ".png") == 0) mime = "image/png";
            else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) mime = "image/jpeg";
            else if (strcasecmp(dot, ".gif") == 0) mime = "image/gif";
            else if (strcasecmp(dot, ".svg") == 0) mime = "image/svg+xml";
            else if (strcasecmp(dot, ".txt") == 0) mime = "text/plain; charset=utf-8";
        }

        // Generate cache headers
        char etag[64];
        snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)file->last_mod, (unsigned long)file->size);
        char last_mod_buf[64];
        cwist_http_format_date(file->last_mod, last_mod_buf, sizeof(last_mod_buf));

        // Check conditional requests
        bool not_modified = false;
        const char *if_none_match = cwist_http_header_get(req->headers, "If-None-Match");
        if (if_none_match && strcmp(if_none_match, etag) == 0) {
            not_modified = true;
        } else {
            const char *if_modified_since = cwist_http_header_get(req->headers, "If-Modified-Since");
            if (if_modified_since) {
                time_t ims = cwist_http_parse_date(if_modified_since);
                if (ims != (time_t)-1 && file->last_mod <= ims) {
                    not_modified = true;
                }
            }
        }

        if (not_modified) {
            res->status_code = CWIST_HTTP_NOT_MODIFIED; // 304
            cwist_http_header_add(&res->headers, "ETag", etag);
            cwist_http_header_add(&res->headers, "Last-Modified", last_mod_buf);
            cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
            cwist_sstring_assign(res->body, "");
        } else if (req->method == CWIST_HTTP_HEAD) {
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", file->size);
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_http_header_add(&res->headers, "Content-Type", mime);
            cwist_http_header_add(&res->headers, "ETag", etag);
            cwist_http_header_add(&res->headers, "Last-Modified", last_mod_buf);
            cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
            cwist_sstring_assign(res->body, "");
        } else if (file->data && file->node) {
            // ZERO COPY
            ttak_mem_node_acquire(file->node);
            cwist_http_response_set_body_ptr_managed(res, file->data, file->size, cwist_static_release_body, file->node);
            
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", file->size);
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_http_header_add(&res->headers, "Content-Type", mime);
            cwist_http_header_add(&res->headers, "ETag", etag);
            cwist_http_header_add(&res->headers, "Last-Modified", last_mod_buf);
            cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
        } else {
            res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            cwist_sstring_assign(res->body, "Static buffer missing");
        }
        if (!not_modified) {
            res->status_code = CWIST_HTTP_OK;
        }
    } else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not Found");
    } else if (ferr.error.err_i16 == -EISDIR) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Directory listing not allowed");
    } else if (ferr.error.err_i16 == -EFBIG) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Static file too large");
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Failed to read static file");
    }
}
#include <limits.h>
#include <errno.h>

/**
 * @brief Allocate and initialize the top-level CWIST application object.
 * @return Newly created application, or NULL when allocation fails.
 */
static cwist_error_t cwist_app_refresh_https_context(cwist_app *app) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !app->cert_path || !app->key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    if (app->ssl_ctx) {
        cwist_https_destroy_context(app->ssl_ctx);
        app->ssl_ctx = NULL;
    }

    cwist_https_options options = {
        .enable_http2 = app->use_https2,
        .enable_http3 = app->use_https3
    };
    return cwist_https_init_context_with_options(&app->ssl_ctx,
                                                 app->cert_path,
                                                 app->key_path,
                                                 &options,
                                                 app);
}

static void static_ssl_http1_handler(cwist_https_connection *conn, void *ctx);
static void static_ssl_http2_handler(cwist_https_connection *conn, void *ctx);

static void cwist_app_refresh_https_request_handler(cwist_app *app) {
    if (!app) return;
    app->https_request_handler = app->use_https2 ? static_ssl_http2_handler : static_ssl_http1_handler;
}

/**
 * @brief Initialize or refresh the HTTP/3 context based on current settings.
 */
static cwist_error_t cwist_app_refresh_http3_context(cwist_app *app) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = -1;
        return err;
    }

    if (app->h3_ctx) {
        cwist_http3_destroy_context(app->h3_ctx);
        app->h3_ctx = NULL;
    }

    if (app->use_https3 || app->use_http3) {
        if (app->use_https3 && app->cert_path && app->key_path) {
            err = cwist_http3_init_context(&app->h3_ctx, app->cert_path, app->key_path);
        } else if (app->use_http3) {
            err = cwist_http3_init_context_ephemeral(&app->h3_ctx);
        } else {
            err.error.err_i16 = -1; // Missing config for strict https3
        }
        if (app->h3_ctx && app->wt_handler) {
            cwist_http3_set_webtransport_handler(app->h3_ctx, app->wt_handler);
        }
    } else {
        err.error.err_i16 = 0;
    }
    return err;
}

cwist_app *cwist_app_create(void) {
    cwist_app *app = (cwist_app *)malloc(sizeof(cwist_app));
    if (!app) return NULL;
    
    app->port = 8080;
    app->use_ssl = false;
    app->cert_path = NULL;
    app->key_path = NULL;
    app->router = cwist_route_table_create();
    if (!app->router) {
        free(app);
        return NULL;
    }
    app->middlewares = NULL;
    app->ssl_ctx = NULL;
    app->error_handler = NULL;
    app->static_dirs = NULL;
    app->db = NULL;
    app->db_path = NULL;
    app->nuke_enabled = false;
    app->max_mem_space = 0;
    app->mem_manager = NULL;
    app->bdr_ctx = cwist_bdr_create();
    app->pqc_layer_enabled = false;
    app->tls_groups = NULL;
    app->wt_handler = NULL;

    app->session_secret = NULL;
    app->session_name = NULL;
    app->session_max_age = 0;
    app->db_pool = NULL;
    app->redis_pool = NULL;
    app->scheduler = NULL;
    app->grpc_routes = NULL;

    cwist_app_refresh_https_request_handler(app);
    
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
    cwist_multiport_destroy_owned_subapps(app);
    cwist_multiport_unlink_app(app);
    if (app->cert_path) cwist_free(app->cert_path);
    if (app->key_path) cwist_free(app->key_path);
    if (app->tls_groups) cwist_free(app->tls_groups);
    if (app->ssl_ctx) cwist_https_destroy_context(app->ssl_ctx);

    cwist_route_table_destroy(app->router);

    cwist_middleware_node *curr_m = app->middlewares;
    while (curr_m) {
        cwist_middleware_node *next = curr_m->next;
        free(curr_m);
        curr_m = next;
    }

    cwist_error_handler_entry *curr_e = app->error_handlers;
    while (curr_e) {
        cwist_error_handler_entry *next = curr_e->next;
        cwist_free(curr_e);
        curr_e = next;
    }

    if (app->config) cwist_config_destroy(app->config);
    if (app->logger) cwist_logger_destroy(app->logger);
    if (app->rdbms) {
        cwist_rdbms_runtime *rt = app->rdbms;
        if (rt->host) cwist_free(rt->host);
        cwist_free(rt);
    }

    cwist_static_dir *curr_s = app->static_dirs;
    while (curr_s) {
        cwist_static_dir *next = curr_s->next;
        free(curr_s->url_prefix);
        free(curr_s->fs_root);
        free(curr_s);
        curr_s = next;
    }

    if (app->db) {
        cwist_db_close(app->db);
    }
    if (app->db_path) {
        free(app->db_path);
    }

    if (app->session_secret) cwist_free(app->session_secret);
    if (app->session_name) cwist_free(app->session_name);

    if (app->db_pool) {
        cwist_db_pool_destroy((cwist_db_pool_t *)app->db_pool);
    }
    if (app->redis_pool) {
        cwist_redis_pool_destroy((cwist_redis_pool_t *)app->redis_pool);
    }
    if (app->scheduler) {
        cwist_scheduler_destroy((cwist_scheduler_t *)app->scheduler);
    }
    cwist_grpc_routes_destroy(app);

    cwist_free(app);
}

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

static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data) {
    mw_executor_ctx ctx = { app->middlewares, final_handler, handler_data };
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

void cwist_app_use_pqc_layer(cwist_app *app, bool enabled)
{
    if (!app) return;
    app->pqc_layer_enabled = enabled;
}

void cwist_app_set_tls_groups(cwist_app *app, const char *groups)
{
    if (!app) return;
    if (app->tls_groups) {
        cwist_free(app->tls_groups);
        app->tls_groups = NULL;
    }
    if (groups) {
        app->tls_groups = cwist_strdup(groups);
    }
}

cwist_error_t cwist_app_use_https2(cwist_app *app, bool enabled) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_https2 = enabled;
    cwist_app_refresh_https_request_handler(app);
    err.error.err_i16 = 0;

    if (!app->use_ssl || !app->cert_path || !app->key_path) {
        return err;
    }

    return cwist_app_refresh_https_context(app);
}

cwist_error_t cwist_app_use_https3(cwist_app *app, bool enabled) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_https3 = enabled;
    err.error.err_i16 = 0;

    if (!app->use_ssl || !app->cert_path || !app->key_path) {
        return err;
    }

    cwist_app_refresh_http3_context(app);
    return cwist_app_refresh_https_context(app);
}

cwist_error_t cwist_app_use_http2(cwist_app *app, bool enabled) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_http2 = enabled;
    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_app_use_http3(cwist_app *app, bool enabled) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_http3 = enabled;
    err.error.err_i16 = 0;
    return cwist_app_refresh_http3_context(app);
}

void cwist_app_use_webtransport(cwist_app *app, cwist_webtransport_handler_func handler)
{
    if (!app) return;
    app->wt_handler = handler;
    if (app->h3_ctx) {
        cwist_http3_set_webtransport_handler(app->h3_ctx, handler);
    }
}

/**
 * @brief Open a SQLite database and attach it as the shared application handle.
 * @param app Application being configured.
 * @param db_path Filesystem path or SQLite URI to open.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !db_path) {
        err.error.err_i16 = -1;
        return err;
    }

    cwist_db *db = NULL;
    err = cwist_db_open(&db, db_path);
    if (err.error.err_i16 < 0) {
        return err;
    }

    if (app->db) {
        cwist_db_close(app->db);
    }
    if (app->db_path) {
        free(app->db_path);
    }

    app->db = db;
    app->db_path = strdup(db_path);
    return err;
}

cwist_db *cwist_app_get_db(cwist_app *app) {
    if (!app) return NULL;
    return app->db;
}

cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !url_prefix || !directory) {
        err.error.err_i16 = -1;
        return err;
    }

    char *normalized = cwist_normalize_prefix(url_prefix);
    if (!normalized) {
        err.error.err_i16 = -1;
        return err;
    }

    char *resolved = cwist_normalize_directory(directory);
    if (!resolved) {
        free(normalized);
        err.error.err_i16 = -1;
        return err;
    }

    cwist_static_dir *entry = (cwist_static_dir *)malloc(sizeof(cwist_static_dir));
    if (!entry) {
        free(normalized);
        free(resolved);
        err.error.err_i16 = -1;
        return err;
    }

    entry->url_prefix = normalized;
    entry->fs_root = resolved;
    entry->next = app->static_dirs;
    app->static_dirs = entry;

    err.error.err_i16 = 0;
    return err;
}

static void add_route(cwist_app *app, const char *path, cwist_http_method_t method, cwist_handler_func handler) {
    if (!app || !app->router || !path) return;
    cwist_route_table_insert(app->router, path, method, handler, NULL);
}

void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_GET, handler);
}

#include <cwist/sys/metrics/metrics.h>

static void cwist_metrics_route_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_metrics_serve_http(res);
}

void cwist_app_enable_metrics(cwist_app *app) {
    if (!app) return;
    cwist_app_get(app, "/metrics", cwist_metrics_route_handler);
}

static void cwist_healthz_route_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_app_healthz(res);
}

static void cwist_liveness_route_handler(cwist_http_request *req, cwist_http_response *res) {
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "{\"status\":\"alive\"}");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

static void cwist_readiness_route_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_app_healthz(res);
}

void cwist_app_enable_healthz(cwist_app *app) {
    if (!app) return;
    cwist_app_get(app, "/healthz", cwist_healthz_route_handler);
    cwist_app_get(app, "/live", cwist_liveness_route_handler);
    cwist_app_get(app, "/ready", cwist_readiness_route_handler);
}

void cwist_app_get_named(cwist_app *app, const char *path, const char *name, cwist_handler_func handler) {
    add_route_named(app, path, name, CWIST_HTTP_GET, handler, CWIST_ENDPOINT_DEFAULT);
}

/**
 * @brief Register a POST handler with default endpoint options.
 * @param app Application being configured.
 * @param path Exact route path.
 * @param handler HTTP handler invoked for matching requests.
 */
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_POST, handler);
}

void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler) {
    if (!app || !app->router || !path) return;
    cwist_route_table_insert(app->router, path, CWIST_HTTP_GET, NULL, handler);
}

static bool match_path(const char *pattern, const char *actual, cwist_query_map *params) {
    char p[256], a[256];
    strncpy(p, pattern, 255);
    strncpy(a, actual, 255);
    p[255] = a[255] = '\0';

    if (params) {
        cwist_query_map_clear(params);
    }

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
    if (!req || !app || !app->router) return;

    cwist_static_request_info static_info = {0};
    if (cwist_prepare_static(app, req, &static_info)) {
        execute_chain(app, req, res, cwist_static_handler, &static_info);
        return;
    }

    const char *path = (req->path && req->path->data) ? req->path->data : "/";
    cwist_route_entry *found_route = cwist_route_table_lookup(app->router, req->method, path);

    if (found_route && req->path_params) {
        cwist_query_map_clear(req->path_params);
    }

    if (!found_route) {
        found_route = cwist_route_table_match_params(app->router, req);
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
            execute_chain(app, req, res, found_route->handler, NULL);
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
    req->app = app;
    req->db = app->db;
    
    cwist_http_response *res = cwist_http_response_create();
    internal_route_handler(app, req, res);
    
    cwist_https_send_response(conn, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void static_http_handler(int client_fd, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    char *read_buf = malloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!read_buf) {
        close(client_fd);
        return;
    }
    size_t buf_len = 0;
    read_buf[0] = '\0';

    while (true) {
        cwist_http_request *req = cwist_http_receive_request(client_fd, read_buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
        if (!req) {
            break;
        }
        req->client_fd = client_fd;
        req->app = app;
        req->db = app->db;

        cwist_http_response *res = cwist_http_response_create();
        if (!res) {
            cwist_http_request_destroy(req);
            break;
        }
        
        internal_route_handler(app, req, res);
        
        bool keep_alive = req->keep_alive && res->keep_alive;
        
        if (!req->upgraded) {
            if (cwist_http_send_response(client_fd, res).error.err_i16 < 0) {
                cwist_http_response_destroy(res);
                cwist_http_request_destroy(req);
                break;
            }
        }
        
        cwist_http_response_destroy(res);
        cwist_http_request_destroy(req);
        
        if (!keep_alive || req->upgraded) {
            break;
        }
    }
    
    free(read_buf);
    close(client_fd);
}

#ifndef CWIST_MULTIPORT_MAX_PORTS
#define CWIST_MULTIPORT_MAX_PORTS 64
#endif

typedef struct cwist_multiport_slot {
    unsigned short port;
    cwist_app *app;
    bool detached;
    struct cwist_multiport_slot *next;
} cwist_multiport_slot;

typedef struct cwist_multiport_group {
    cwist_app *root;
    unsigned short public_port;
    cwist_multiport_slot *slots;
    struct cwist_multiport_group *next;
} cwist_multiport_group;

static cwist_multiport_group *g_cwist_multiport_groups = NULL;

/**
 * @brief Build a counted multiport descriptor from an explicit pointer and length.
 * @param ports Source port array. A zero value stops copying for compatibility.
 * @param count Number of source elements.
 * @return Counted multiport descriptor with valid=false on construction failure.
 */
cwist_multiport_t cwist_create_multiport_from_array(const unsigned short *ports, size_t count) {
    cwist_multiport_t result;
    memset(&result, 0, sizeof(result));
    result.valid = false;

    if (count > CWIST_MULTIPORT_MAX_PORTS) {
        return result;
    }
    if (!ports && count > 0) {
        return result;
    }

    for (size_t i = 0; i < count; i++) {
        if (ports[i] == 0) {
            break;
        }
        result.ports[result.count++] = ports[i];
    }

    result.valid = true;
    return result;
}

/**
 * @brief Find the multiport group attached to a root app.
 * @param root Root application pointer.
 * @return Existing group, or NULL when none exists.
 */
static cwist_multiport_group *cwist_multiport_find_group(cwist_app *root) {
    cwist_multiport_group *group = g_cwist_multiport_groups;
    while (group) {
        if (group->root == root) return group;
        group = group->next;
    }
    return NULL;
}

/**
 * @brief Get or create the multiport group for a root app.
 * @param root Root application pointer.
 * @return Group object, or NULL on allocation failure.
 */
static cwist_multiport_group *cwist_multiport_get_group(cwist_app *root) {
    if (!root) return NULL;
    cwist_multiport_group *group = cwist_multiport_find_group(root);
    if (group) return group;

    group = (cwist_multiport_group *)cwist_alloc(sizeof(*group));
    if (!group) return NULL;
    memset(group, 0, sizeof(*group));
    group->root = root;
    group->next = g_cwist_multiport_groups;
    g_cwist_multiport_groups = group;
    return group;
}

/**
 * @brief Find a per-port multiport slot.
 * @param group Group to inspect.
 * @param port TCP port to match.
 * @return Matching slot, or NULL.
 */
static cwist_multiport_slot *cwist_multiport_find_slot(cwist_multiport_group *group, unsigned short port) {
    if (!group) return NULL;
    cwist_multiport_slot *slot = group->slots;
    while (slot) {
        if (slot->port == port) return slot;
        slot = slot->next;
    }
    return NULL;
}

/**
 * @brief Remove references to an app from every multiport group.
 * @param app Application being destroyed or detached externally.
 */
static void cwist_multiport_unlink_app(cwist_app *app) {
    if (!app) return;
    cwist_multiport_group *group = g_cwist_multiport_groups;
    while (group) {
        cwist_multiport_slot **link = &group->slots;
        while (*link) {
            cwist_multiport_slot *slot = *link;
            if (slot->app == app) {
                *link = slot->next;
                cwist_free(slot);
                continue;
            }
            link = &slot->next;
        }
        group = group->next;
    }
}

/**
 * @brief Destroy sub-apps owned by a root multiport group and remove that group.
 * @param root Root application being destroyed.
 */
static void cwist_multiport_destroy_owned_subapps(cwist_app *root) {
    if (!root) return;
    cwist_multiport_group **link = &g_cwist_multiport_groups;
    while (*link) {
        cwist_multiport_group *group = *link;
        if (group->root != root) {
            link = &group->next;
            continue;
        }

        cwist_multiport_slot *slot = group->slots;
        while (slot) {
            cwist_multiport_slot *next = slot->next;
            if (slot->detached && slot->app && slot->app != root) {
                cwist_app *sub_app = slot->app;
                slot->app = NULL;
                cwist_app_destroy(sub_app);
            }
            cwist_free(slot);
            slot = next;
        }

        *link = group->next;
        cwist_free(group);
        return;
    }
}

/**
 * @brief Clone a route table into an independent table.
 * @param src Source route table.
 * @return Deep-cloned table, or NULL on failure.
 */
static cwist_route_table *cwist_route_table_clone(cwist_route_table *src) {
    if (!src) return cwist_route_table_create();
    cwist_route_table *dst = cwist_route_table_create();
    if (!dst) return NULL;

    for (size_t i = 0; i < src->bucket_count; i++) {
        for (cwist_route_entry *entry = src->buckets[i]; entry; entry = entry->next) {
            cwist_route_table_insert(dst, entry->path, entry->name, entry->method, entry->handler, entry->ws_handler, entry->opts);
        }
    }
    for (cwist_route_entry *entry = src->param_routes; entry; entry = entry->next) {
        cwist_route_table_insert(dst, entry->path, entry->name, entry->method, entry->handler, entry->ws_handler, entry->opts);
    }
    return dst;
}

/**
 * @brief Clone middleware nodes while preserving callback order.
 * @param src Source middleware list.
 * @return Cloned list, or NULL for an empty source.
 */
static cwist_middleware_node *cwist_middleware_clone(cwist_middleware_node *src) {
    cwist_middleware_node *head = NULL;
    cwist_middleware_node **tail = &head;
    while (src) {
        cwist_middleware_node *node = (cwist_middleware_node *)cwist_alloc(sizeof(*node));
        if (!node) break;
        node->func = src->func;
        node->next = NULL;
        *tail = node;
        tail = &node->next;
        src = src->next;
    }
    return head;
}

/**
 * @brief Clone per-status error handlers.
 * @param src Source error-handler list.
 * @return Cloned list, or NULL for an empty source.
 */
static cwist_error_handler_entry *cwist_error_handlers_clone(cwist_error_handler_entry *src) {
    cwist_error_handler_entry *head = NULL;
    cwist_error_handler_entry **tail = &head;
    while (src) {
        cwist_error_handler_entry *entry = (cwist_error_handler_entry *)cwist_alloc(sizeof(*entry));
        if (!entry) break;
        entry->status_code = src->status_code;
        entry->handler = src->handler;
        entry->next = NULL;
        *tail = entry;
        tail = &entry->next;
        src = src->next;
    }
    return head;
}

/**
 * @brief Clone static directory mappings into an independent list.
 * @param src Source static-dir list.
 * @return Cloned list, or NULL for an empty source.
 */
static cwist_static_dir *cwist_static_dirs_clone(cwist_static_dir *src) {
    cwist_static_dir *head = NULL;
    cwist_static_dir **tail = &head;
    while (src) {
        cwist_static_dir *entry = (cwist_static_dir *)cwist_alloc(sizeof(*entry));
        if (!entry) break;
        entry->url_prefix = cwist_strdup(src->url_prefix);
        entry->fs_root = cwist_strdup(src->fs_root);
        entry->cache_control = src->cache_control ? cwist_strdup(src->cache_control) : NULL;
        entry->next = NULL;
        if (!entry->url_prefix || !entry->fs_root || (src->cache_control && !entry->cache_control)) {
            cwist_free(entry->url_prefix);
            cwist_free(entry->fs_root);
            cwist_free(entry->cache_control);
            cwist_free(entry);
            break;
        }
        *tail = entry;
        tail = &entry->next;
        src = src->next;
    }
    return head;
}

/**
 * @brief Fork a root app into an independent sub-app for one port.
 * @param src Root application to clone.
 * @return Tunable sub-application, or NULL on failure.
 */
static cwist_app *cwist_app_clone_for_multiport(cwist_app *src) {
    if (!src) return NULL;
    cwist_app *dst = cwist_app_create();
    if (!dst) return NULL;

    cwist_route_table *router = cwist_route_table_clone(src->router);
    if (!router) {
        cwist_app_destroy(dst);
        return NULL;
    }
    cwist_route_table_destroy(dst->router);
    dst->router = router;

    dst->port = src->port;
    dst->use_http2 = src->use_http2;
    dst->use_https2 = src->use_https2;
    dst->use_http3 = src->use_http3;
    dst->use_https3 = src->use_https3;
    dst->error_handler = src->error_handler;
    dst->max_mem_space = src->max_mem_space;
    dst->pqc_layer_enabled = src->pqc_layer_enabled;
    if (src->tls_groups) {
        dst->tls_groups = cwist_strdup(src->tls_groups);
    }
    dst->wt_handler = src->wt_handler;

    dst->middlewares = cwist_middleware_clone(src->middlewares);
    dst->error_handlers = cwist_error_handlers_clone(src->error_handlers);
    dst->static_dirs = cwist_static_dirs_clone(src->static_dirs);
    if (cwist_grpc_routes_clone(dst, src) != 0) {
        cwist_app_destroy(dst);
        return NULL;
    }

    if (src->use_ssl && src->cert_path && src->key_path) {
        dst->use_https2 = src->use_https2;
        dst->use_https3 = src->use_https3;
        cwist_app_use_https(dst, src->cert_path, src->key_path);
    }
    if (src->use_http3) {
        cwist_app_use_http3(dst, true);
    }
    if (src->db_path && !src->nuke_enabled) {
        cwist_app_use_db(dst, src->db_path);
    }
    cwist_app_refresh_https_request_handler(dst);
    return dst;
}

/**
 * @brief Detach one additional multiport port into an independent sub-app.
 * @param app_ref Address of the root app pointer.
 * @param port Additional TCP port to detach.
 * @return Detached sub-app for per-port tuning, or NULL on failure.
 */
cwist_app *cwist_multiport_get_app(cwist_app **app_ref, unsigned short port) {
    cwist_app *root = app_ref ? *app_ref : NULL;
    if (!root || port == 0 || port == (unsigned short)root->port) return NULL;

    cwist_multiport_group *group = cwist_multiport_get_group(root);
    if (!group) return NULL;

    cwist_multiport_slot *slot = cwist_multiport_find_slot(group, port);
    if (slot) {
        return slot->detached ? slot->app : NULL;
    }

    cwist_app *sub_app = cwist_app_clone_for_multiport(root);
    if (!sub_app) return NULL;
    sub_app->port = port;

    slot = (cwist_multiport_slot *)cwist_alloc(sizeof(*slot));
    if (!slot) {
        cwist_app_destroy(sub_app);
        return NULL;
    }
    slot->port = port;
    slot->app = sub_app;
    slot->detached = true;
    slot->next = group->slots;
    group->slots = slot;
    return sub_app;
}

typedef struct cwist_multiport_http_client {
    int client_fd;
    cwist_app *app;
} cwist_multiport_http_client;

static void *cwist_multiport_http_client_thread(void *arg) {
    cwist_multiport_http_client *client = (cwist_multiport_http_client *)arg;
    if (!client) return NULL;
    static_http_handler(client->client_fd, client->app);
    free(client);
    return NULL;
}

static int cwist_multiport_add_port(unsigned short *ports, size_t *count, unsigned short port) {
    if (!ports || !count || port == 0) return -1;
    for (size_t i = 0; i < *count; i++) {
        if (ports[i] == port) return 0;
    }
    if (*count >= CWIST_MULTIPORT_MAX_PORTS) return -1;
    ports[*count] = port;
    (*count)++;
    return 0;
}

static int cwist_multiport_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void cwist_multiport_close_all(struct pollfd *pfds, size_t count) {
    if (!pfds) return;
    for (size_t i = 0; i < count; i++) {
        if (pfds[i].fd >= 0) {
            close(pfds[i].fd);
            pfds[i].fd = -1;
        }
    }
}

static int cwist_multiport_start_clear_client(int client_fd, cwist_app *app) {
    cwist_multiport_http_client *payload = malloc(sizeof(*payload));
    if (!payload) {
        close(client_fd);
        return -1;
    }

    payload->client_fd = client_fd;
    payload->app = app;

    pthread_t tid;
    if (pthread_create(&tid, NULL, cwist_multiport_http_client_thread, payload) != 0) {
        close(client_fd);
        free(payload);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/**
 * @brief Check whether a counted multiport descriptor contains a port.
 * @param ports Counted multiport descriptor.
 * @param port TCP port to find.
 * @return true when the port is present.
 */
static bool cwist_multiport_contains(cwist_multiport_t ports, unsigned short port) {
    for (size_t i = 0; i < ports.count; i++) {
        if (ports.ports[i] == port) return true;
    }
    return false;
}

/**
 * @brief Resolve the app assigned to a bound port.
 * @param group Multiport group for the root app.
 * @param root Root app used by non-detached ports.
 * @param port Bound TCP port.
 * @return Detached sub-app for the port, or root when not detached.
 */
static cwist_app *cwist_multiport_bound_app(cwist_multiport_group *group, cwist_app *root, unsigned short port) {
    cwist_multiport_slot *slot = cwist_multiport_find_slot(group, port);
    if (slot && slot->detached && slot->app) {
        return slot->app;
    }
    return root;
}

/**
 * @brief Facade listener that serves one cwist_app over multiple TCP ports.
 *
 * The additional ports are counted in cwist_multiport_t so callers can pass a
 * normal C array through cwist_create_multiport().
 */
int cwist_app_multiport(cwist_app **app_ref, unsigned short public_port, cwist_multiport_t ports) {
    signal(SIGPIPE, SIG_IGN);
    cwist_shutdown_install_handlers();
    cwist_app_tune_system();

    cwist_app *app = app_ref ? *app_ref : NULL;
    if (!app || public_port == 0 || !ports.valid) return -1;
    app->port = public_port;

    cwist_multiport_group *group = cwist_multiport_get_group(app);
    if (!group) return -1;
    group->public_port = public_port;

    for (cwist_multiport_slot *slot = group->slots; slot; slot = slot->next) {
        if (slot->detached && slot->port == public_port) {
            fprintf(stderr, "cwist_multiport_get_app cannot detach the public/default port %hu.\n", public_port);
            return -1;
        }
        if (slot->detached && !cwist_multiport_contains(ports, slot->port)) {
            fprintf(stderr, "Detached multiport sub-app port %hu is not present in the multiport descriptor.\n", slot->port);
            return -1;
        }
    }

    if (app->use_ssl && app->use_http2) {
        fprintf(stderr, "Assertion failed: Cannot use cleartext HTTP/2 and HTTPS on the same port.\n");
        return -1;
    }
    if (!app->use_ssl && app->use_https2) {
        fprintf(stderr, "Assertion failed: Cannot use HTTPS/2 without configuring SSL via cwist_app_use_https.\n");
        return -1;
    }
    if (app->use_ssl && !app->ssl_ctx) {
        fprintf(stderr, "SSL enabled but context not initialized.\n");
        return -1;
    }

    if (!app->mem_manager) {
        cwist_mem_init(app);
    }
    if (app->mem_manager && !app->mem_manager->watcher_running) {
        app->mem_manager->watcher_running = true;
        pthread_create(&app->mem_manager->watcher_thread, NULL, cwist_mem_watcher, app);
    }

    unsigned short bind_ports[CWIST_MULTIPORT_MAX_PORTS];
    size_t port_count = 0;
    if (cwist_multiport_add_port(bind_ports, &port_count, public_port) != 0) {
        return -1;
    }
    for (size_t i = 0; i < ports.count; i++) {
        if (cwist_multiport_add_port(bind_ports, &port_count, ports.ports[i]) != 0) {
            fprintf(stderr, "cwist_app_multiport supports up to %d total ports.\n", CWIST_MULTIPORT_MAX_PORTS);
            return -1;
        }
    }

    struct pollfd pfds[CWIST_MULTIPORT_MAX_PORTS];
    for (size_t i = 0; i < port_count; i++) {
        pfds[i].fd = -1;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    for (size_t i = 0; i < port_count; i++) {
        struct sockaddr_in addr;
        int fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", bind_ports[i], 32768);
        if (fd < 0) {
            perror("Failed to bind multiport listener");
            cwist_multiport_close_all(pfds, i);
            return -1;
        }
        if (cwist_multiport_set_nonblocking(fd) != 0) {
            perror("Failed to set multiport listener non-blocking");
            close(fd);
            cwist_multiport_close_all(pfds, i);
            return -1;
        }
        pfds[i].fd = fd;
        printf("CWIST multiport facade listening on TCP port %hu\n", bind_ports[i]);
    }

    if (app->use_ssl && https_pool_init() != 0) {
        fprintf(stderr, "Failed to initialize HTTPS worker pool.\n");
        cwist_multiport_close_all(pfds, port_count);
        return -1;
    }

    g_cwist_listen_fd = pfds[0].fd;
    printf("CWIST App running on %zu TCP ports via multiport facade (SSL: %s)\n",
           port_count, app->use_ssl ? "On" : "Off");

    while (atomic_load(&g_cwist_running)) {
        int ready = poll(pfds, (nfds_t)port_count, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("cwist_app_multiport poll");
            break;
        }
        if (ready == 0) continue;

        for (size_t i = 0; i < port_count; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;

            while (atomic_load(&g_cwist_running)) {
                struct sockaddr_in peer;
                socklen_t peer_len = sizeof(peer);
                int client_fd = accept(pfds[i].fd, (struct sockaddr *)&peer, &peer_len);
                if (client_fd < 0) {
                    int accept_err = errno;
                    if (accept_err == EAGAIN || accept_err == EWOULDBLOCK || accept_err == EINTR) break;
                    if (accept_err == EBADF || accept_err == EINVAL || accept_err == ENOTSOCK) {
                        atomic_store(&g_cwist_running, 0);
                        break;
                    }
                    continue;
                }

                cwist_app *port_app = cwist_multiport_bound_app(group, app, bind_ports[i]);
                if (port_app->use_ssl) {
                    https_pool_submit(client_fd, port_app->ssl_ctx, static_ssl_handler, port_app);
                } else {
                    cwist_multiport_start_clear_client(client_fd, port_app);
                }
            }
        }
    }

    if (app->use_ssl) {
        https_pool_destroy();
    }
    cwist_multiport_close_all(pfds, port_count);
    g_cwist_listen_fd = -1;

    if (app->mem_manager) {
        app->mem_manager->watcher_running = false;
    }

    printf("[CWIST] Multiport facade shutdown complete.\n");
    return 0;
}
struct h3_thread_payload {
    int udp_fd;
    cwist_app *app;
};

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
