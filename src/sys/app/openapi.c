/**
 * @file openapi.c
 * @brief Runtime OpenAPI 3.1 endpoint (#408).
 *
 * cwist_app_enable_openapi() mounts a GET route whose context is the app's
 * document state: metadata plus cwist_openapi_annotate() entries.  Every
 * request walks the live route table, so a route added anywhere shows up with
 * its path and method; annotations only add a summary and tags.
 *
 * The route owns the state and frees it with the app.  A small registry maps
 * the app back to its state so cwist_openapi_annotate() can find it.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/openapi.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>

#include "routes_internal.h"

typedef struct openapi_note {
    cwist_http_method_t method;
    char *path;
    char *summary;
    char *tags;
    struct openapi_note *next;
} openapi_note;

typedef struct openapi_doc {
    cwist_app *app;
    char *json_path;
    char *title;
    char *version;
    char *description;
    char *server_url;
    openapi_note *notes;
    struct openapi_doc *next;
} openapi_doc;

static openapi_doc *g_docs;
static pthread_mutex_t g_docs_mu = PTHREAD_MUTEX_INITIALIZER;

static char *dup_or_null(const char *s) {
    return s ? cwist_strdup(s) : NULL;
}

static cwist_http_method_t method_from_string(const char *method) {
    static const cwist_http_method_t all[] = {
        CWIST_HTTP_GET,  CWIST_HTTP_POST, CWIST_HTTP_PUT,     CWIST_HTTP_DELETE,
        CWIST_HTTP_PATCH, CWIST_HTTP_HEAD, CWIST_HTTP_OPTIONS,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcasecmp(method, cwist_http_method_to_string(all[i])) == 0) return all[i];
    }
    return CWIST_HTTP_UNKNOWN;
}

/* Caller holds g_docs_mu. */
static openapi_doc *find_doc(const cwist_app *app) {
    for (openapi_doc *d = g_docs; d; d = d->next) {
        if (d->app == app) return d;
    }
    return NULL;
}

static const openapi_note *find_note(const openapi_doc *doc, cwist_http_method_t method,
                                     const char *path) {
    for (const openapi_note *n = doc->notes; n; n = n->next) {
        if (n->method == method && strcmp(n->path, path) == 0) return n;
    }
    return NULL;
}

static void doc_destroy(void *ctx) {
    openapi_doc *doc = ctx;
    pthread_mutex_lock(&g_docs_mu);
    for (openapi_doc **p = &g_docs; *p; p = &(*p)->next) {
        if (*p == doc) {
            *p = doc->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_docs_mu);

    while (doc->notes) {
        openapi_note *n = doc->notes;
        doc->notes = n->next;
        cwist_free(n->path);
        cwist_free(n->summary);
        cwist_free(n->tags);
        cwist_free(n);
    }
    cwist_free(doc->json_path);
    cwist_free(doc->title);
    cwist_free(doc->version);
    cwist_free(doc->description);
    cwist_free(doc->server_url);
    cwist_free(doc);
}

/* "/users/:id" -> "/users/{id}", with one required path parameter per segment. */
static char *openapi_path(const char *route, cJSON *params) {
    size_t len = strlen(route);
    char *out = cwist_alloc(len * 2 + 1);
    if (!out) return NULL;
    char *w = out;
    for (const char *r = route; *r;) {
        if (*r != ':' || (r != route && r[-1] != '/')) {
            *w++ = *r++;
            continue;
        }
        const char *name = ++r;
        while (*r && *r != '/') r++;
        *w++ = '{';
        memcpy(w, name, (size_t)(r - name));
        w += r - name;
        *w++ = '}';

        char *pname = cwist_strndup(name, (size_t)(r - name));
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", pname ? pname : "");
        cwist_free(pname);
        cJSON_AddStringToObject(p, "in", "path");
        cJSON_AddBoolToObject(p, "required", 1);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "schema"), "type", "string");
        cJSON_AddItemToArray(params, p);
    }
    *w = '\0';
    return out;
}

typedef struct {
    const openapi_doc *doc;
    cJSON *paths;
} build_ctx;

static void add_operation(void *ctx, cwist_http_method_t method, const char *route) {
    build_ctx *b = ctx;
    if (method == CWIST_HTTP_CONNECT || method == CWIST_HTTP_UNKNOWN) return;
    if (method == CWIST_HTTP_GET && strcmp(route, b->doc->json_path) == 0) return;

    cJSON *params = cJSON_CreateArray();
    char *path = openapi_path(route, params);
    if (!path) {
        cJSON_Delete(params);
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(b->paths, path);
    if (!item) item = cJSON_AddObjectToObject(b->paths, path);
    cwist_free(path);

    char verb[16];
    snprintf(verb, sizeof(verb), "%s", cwist_http_method_to_string(method));
    for (char *c = verb; *c; c++) *c = (char)(*c | 0x20);

    cJSON *op = cJSON_AddObjectToObject(item, verb);
    const openapi_note *note = find_note(b->doc, method, route);
    if (note && note->summary) cJSON_AddStringToObject(op, "summary", note->summary);
    if (note && note->tags) {
        cJSON *tags = cJSON_AddArrayToObject(op, "tags");
        char *list = cwist_strdup(note->tags);
        char *save = NULL;
        for (char *t = list ? strtok_r(list, ",", &save) : NULL; t; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            if (*t) cJSON_AddItemToArray(tags, cJSON_CreateString(t));
        }
        cwist_free(list);
    }
    if (cJSON_GetArraySize(params) > 0)
        cJSON_AddItemToObject(op, "parameters", params);
    else
        cJSON_Delete(params);
    cJSON *ok = cJSON_AddObjectToObject(cJSON_AddObjectToObject(op, "responses"), "200");
    cJSON_AddStringToObject(ok, "description", "OK");
}

static void openapi_handler(void *ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    const openapi_doc *doc = ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "openapi", "3.1.0");
    cJSON *info = cJSON_AddObjectToObject(root, "info");
    cJSON_AddStringToObject(info, "title", doc->title ? doc->title : "CWIST API");
    cJSON_AddStringToObject(info, "version", doc->version ? doc->version : "1.0.0");
    if (doc->description) cJSON_AddStringToObject(info, "description", doc->description);
    cJSON *server = cJSON_CreateObject();
    cJSON_AddStringToObject(server, "url", doc->server_url ? doc->server_url : "/");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "servers"), server);

    build_ctx b = {.doc = doc, .paths = cJSON_AddObjectToObject(root, "paths")};
    pthread_mutex_lock(&g_docs_mu);
    cwist_app_foreach_route(doc->app, add_operation, &b);
    pthread_mutex_unlock(&g_docs_mu);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_sstring_assign(res->body, json);
    cJSON_free(json);
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

void cwist_app_enable_openapi(cwist_app *app, const char *json_path,
                              const cwist_openapi_meta_t *meta) {
    if (!app) return;
    pthread_mutex_lock(&g_docs_mu);
    bool enabled = find_doc(app) != NULL;
    pthread_mutex_unlock(&g_docs_mu);
    if (enabled) return;

    openapi_doc *doc = cwist_alloc(sizeof(*doc));
    if (!doc) return;
    doc->app = app;
    doc->json_path = cwist_strdup(json_path ? json_path : "/openapi.json");
    if (meta) {
        doc->title = dup_or_null(meta->title);
        doc->version = dup_or_null(meta->version);
        doc->description = dup_or_null(meta->description);
        doc->server_url = dup_or_null(meta->server_url);
    }
    if (!doc->json_path) {
        cwist_free(doc);
        return;
    }

    pthread_mutex_lock(&g_docs_mu);
    doc->next = g_docs;
    g_docs = doc;
    pthread_mutex_unlock(&g_docs_mu);

    cwist_error_t err = cwist_app_get_ex(app, doc->json_path, openapi_handler, doc, doc_destroy);
    if (!cwist_error_is_ok(&err)) doc_destroy(doc);
}

void cwist_openapi_annotate(cwist_app *app, const char *method, const char *path,
                            const char *summary, const char *tags) {
    if (!app || !method || !path) return;
    cwist_http_method_t m = method_from_string(method);
    if (m == CWIST_HTTP_UNKNOWN) return;

    pthread_mutex_lock(&g_docs_mu);
    openapi_doc *doc = find_doc(app);
    openapi_note *note = doc ? (openapi_note *)find_note(doc, m, path) : NULL;
    if (doc && !note && (note = cwist_alloc(sizeof(*note)))) {
        note->method = m;
        note->path = cwist_strdup(path);
        if (!note->path) {
            cwist_free(note);
            note = NULL;
        } else {
            note->next = doc->notes;
            doc->notes = note;
        }
    }
    if (note) {
        cwist_free(note->summary);
        cwist_free(note->tags);
        note->summary = dup_or_null(summary);
        note->tags = dup_or_null(tags);
    }
    pthread_mutex_unlock(&g_docs_mu);
}
