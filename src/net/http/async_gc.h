/* Private deferred-exchange ownership helpers. Keep in sync with the HTTP,
 * sstring, query-map and session destructors. Call on the allocating thread,
 * while the graph is exclusively owned; disown never frees or adopts memory. */
#ifndef CWIST_HTTP_ASYNC_GC_H
#define CWIST_HTTP_ASYNC_GC_H

#include <cwist/core/mem/arena.h>
#include <cwist/core/mem/gc.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/session.h>

static inline void cwist_http_async_disown_string(cwist_sstring *str) {
    if (!str) return;
    if (!str->borrows_buffer) cwist_gc_scope_disown(str->data);
    if (str->owns_storage) cwist_gc_scope_disown(str);
}

static inline void cwist_http_async_disown_headers(cwist_http_header_node *node) {
    for (; node; node = node->next) {
        cwist_http_async_disown_string(node->key);
        cwist_http_async_disown_string(node->value);
        if (!node->arena_owned) cwist_gc_scope_disown(node);
    }
}

static inline void cwist_http_async_disown_map(cwist_query_map *map) {
    /* Arena maps have no heap fallback and their destructor is a no-op. */
    if (!map || map->arena) return;
    if (map->buckets) {
        for (size_t i = 0; i < map->size; ++i) {
            for (cwist_query_bucket *node = map->buckets[i]; node; node = node->next) {
                cwist_gc_scope_disown(node->key);
                cwist_gc_scope_disown(node->value);
                cwist_gc_scope_disown(node);
            }
        }
        cwist_gc_scope_disown(map->buckets);
    }
    cwist_gc_scope_disown(map);
}

/* Defined by session.c, which alone knows the opaque session layout. */
void cwist_http_async_disown_session(cwist_session_t *session);

static inline void cwist_http_async_disown_request(cwist_http_request *req) {
    if (!req || !cwist_full_gc_enabled()) return;
    cwist_http_async_disown_string(req->path);
    cwist_http_async_disown_string(req->query);
    cwist_http_async_disown_map(req->query_params);
    cwist_http_async_disown_map(req->path_params);
    cwist_http_async_disown_string(req->version);
    cwist_http_async_disown_string(req->body);
    cwist_http_async_disown_map(req->flash);
    cwist_http_async_disown_session(req->session);
    cwist_gc_scope_disown(req->csrf_token);
    cwist_http_async_disown_headers(req->headers);
    if (!req->arena || !cwist_arena_owns(req->arena, req)) cwist_gc_scope_disown(req);
    /* app/db/protocol/middleware state are borrowed, not request-owned. */
}

static inline void cwist_http_async_disown_response(cwist_http_response *res) {
    if (!res || !cwist_full_gc_enabled()) return;
    cwist_http_async_disown_string(res->version);
    cwist_http_async_disown_string(res->status_text);
    cwist_http_async_disown_string(res->body);
    cwist_http_async_disown_headers(res->headers);
    cwist_gc_scope_disown(res->alt_svc);
    /* Cleanup, not GC, owns a managed payload. Do not inspect the opaque
     * context or change the callback; caller transfers its dependencies. */
    if (res->is_ptr_body && res->ptr_body_cleanup) cwist_gc_scope_disown((void *)res->ptr_body);
    if (!res->arena || !cwist_arena_owns(res->arena, res)) cwist_gc_scope_disown(res);
}

#endif /* CWIST_HTTP_ASYNC_GC_H */
