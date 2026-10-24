/**
 * @file openapi.c
 * @brief Runtime OpenAPI 3.1 endpoint (#408).
 *
 * cwist_app_enable_openapi() registers a GET handler that serves an
 * OpenAPI 3.1 JSON document.  Per-route metadata is collected via
 * cwist_openapi_annotate() before cwist_app_listen() is called; the
 * handler builds the document on the first request and caches it for
 * subsequent ones.
 *
 * The store is a fixed-size array capped at CWIST_OPENAPI_MAX_ROUTES.
 * A production implementation would grow it dynamically; for v4.1 the
 * cap is intentionally visible so users know to bump it when needed.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/openapi.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#define CWIST_OPENAPI_MAX_ROUTES 256
#define CWIST_OPENAPI_JSON_CAP   65536

typedef struct {
    char method[16];
    char path[256];
    char summary[256];
    char tags[128];
} openapi_route_t;

typedef struct {
    cwist_openapi_meta_t meta;
    openapi_route_t      routes[CWIST_OPENAPI_MAX_ROUTES];
    size_t               route_count;
    char                *cached_json;
    pthread_mutex_t      mu;
} openapi_state_t;

/* One global state for the lifetime of the process.  Multiple cwist_app
 * instances reuse it; the first call to cwist_app_enable_openapi() wins
 * the metadata. */
static openapi_state_t g_state;
static pthread_once_t  g_init_once = PTHREAD_ONCE_INIT;

static void state_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.mu, NULL);
}

/* Escape a string for JSON (backslash and double-quote only; the inputs are
 * ASCII C identifiers or short human strings, so full Unicode escaping is
 * not required here). */
static size_t json_escape(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') {
            if (n + 3 >= cap) break;
            dst[n++] = '\\';
        }
        dst[n++] = *src;
    }
    dst[n] = '\0';
    return n;
}

/* Build the OpenAPI 3.1 JSON document into a freshly allocated buffer. */
static char *build_json(void) {
    char *buf = (char *)cwist_alloc(CWIST_OPENAPI_JSON_CAP);
    if (!buf) return NULL;

    const cwist_openapi_meta_t *m = &g_state.meta;
    const char *title   = m->title       ? m->title       : "CWIST API";
    const char *version = m->version     ? m->version     : "1.0.0";
    const char *desc    = m->description ? m->description : "";
    const char *srvurl  = m->server_url  ? m->server_url  : "/";

    char etitle[256], eversion[64], edesc[512], esrvurl[256];
    json_escape(title,   etitle,   sizeof(etitle));
    json_escape(version, eversion, sizeof(eversion));
    json_escape(desc,    edesc,    sizeof(edesc));
    json_escape(srvurl,  esrvurl,  sizeof(esrvurl));

    int pos = snprintf(buf, CWIST_OPENAPI_JSON_CAP,
        "{\n"
        "  \"openapi\": \"3.1.0\",\n"
        "  \"info\": {\n"
        "    \"title\": \"%s\",\n"
        "    \"version\": \"%s\"%s%s%s\n"
        "  },\n"
        "  \"servers\": [{\"url\": \"%s\"}],\n"
        "  \"paths\": {",
        etitle, eversion,
        *edesc ? ",\n    \"description\": \"" : "",
        *edesc ? edesc : "",
        *edesc ? "\"" : "",
        esrvurl);

    for (size_t i = 0; i < g_state.route_count && pos < CWIST_OPENAPI_JSON_CAP - 256; i++) {
        const openapi_route_t *r = &g_state.routes[i];
        char epath[256], esummary[256], etags[256];
        json_escape(r->path,    epath,    sizeof(epath));
        json_escape(r->summary, esummary, sizeof(esummary));

        /* Tags array from comma-separated string. */
        char tags_json[256] = "[]";
        if (r->tags[0]) {
            char tmp[128];
            json_escape(r->tags, tmp, sizeof(tmp));
            /* Split on commas for a minimal array rendering. */
            char arr[256];
            int ap = 0;
            arr[ap++] = '[';
            const char *p = tmp;
            while (*p) {
                const char *comma = strchr(p, ',');
                size_t len = comma ? (size_t)(comma - p) : strlen(p);
                ap += snprintf(arr + ap, sizeof(arr) - (size_t)ap,
                               "\"%.*s\"%s", (int)len, p, comma ? "," : "");
                p = comma ? comma + 1 : p + len;
                if (!comma) break;
            }
            arr[ap++] = ']';
            arr[ap] = '\0';
            snprintf(tags_json, sizeof(tags_json), "%s", arr);
        }

        char method_lower[16];
        for (size_t j = 0; j < sizeof(method_lower) - 1 && r->method[j]; j++) {
            method_lower[j] = (char)(r->method[j] | 0x20);
            method_lower[j + 1] = '\0';
        }

        pos += snprintf(buf + pos, (size_t)(CWIST_OPENAPI_JSON_CAP - pos),
            "%s\n    \"%s\": {\n"
            "      \"%s\": {\n"
            "        \"summary\": \"%s\",\n"
            "        \"tags\": %s,\n"
            "        \"responses\": {\"200\": {\"description\": \"OK\"}}\n"
            "      }\n"
            "    }",
            i > 0 ? "," : "",
            epath, method_lower, esummary, tags_json);
    }

    if (pos < CWIST_OPENAPI_JSON_CAP - 8)
        pos += snprintf(buf + pos, (size_t)(CWIST_OPENAPI_JSON_CAP - pos), "\n  }\n}\n");

    return buf;
}

static void openapi_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    pthread_mutex_lock(&g_state.mu);
    if (!g_state.cached_json)
        g_state.cached_json = build_json();
    const char *json = g_state.cached_json
        ? g_state.cached_json
        : "{\"error\":\"openapi build failed\"}";
    cwist_sstring_assign(res->body, json);
    pthread_mutex_unlock(&g_state.mu);
    res->status = 200;
    cwist_http_set_header(res, "Content-Type", "application/json");
}

void cwist_app_enable_openapi(cwist_app *app, const char *json_path,
                              const cwist_openapi_meta_t *meta) {
    if (!app) return;
    pthread_once(&g_init_once, state_init);

    pthread_mutex_lock(&g_state.mu);
    if (meta) {
        g_state.meta.title       = meta->title;
        g_state.meta.version     = meta->version;
        g_state.meta.description = meta->description;
        g_state.meta.server_url  = meta->server_url;
    }
    /* Invalidate any cached document so the next request rebuilds. */
    cwist_free(g_state.cached_json);
    g_state.cached_json = NULL;
    pthread_mutex_unlock(&g_state.mu);

    cwist_app_get(app, json_path ? json_path : "/openapi.json", openapi_handler);
}

void cwist_openapi_annotate(cwist_app *app, const char *method, const char *path,
                            const char *summary, const char *tags) {
    (void)app;
    if (!method || !path) return;
    pthread_once(&g_init_once, state_init);

    pthread_mutex_lock(&g_state.mu);
    if (g_state.route_count < CWIST_OPENAPI_MAX_ROUTES) {
        openapi_route_t *r = &g_state.routes[g_state.route_count++];
        snprintf(r->method,  sizeof(r->method),  "%s", method);
        snprintf(r->path,    sizeof(r->path),    "%s", path);
        snprintf(r->summary, sizeof(r->summary), "%s", summary ? summary : "");
        snprintf(r->tags,    sizeof(r->tags),    "%s", tags ? tags : "");
    }
    /* Invalidate cache so the added route appears on the next request. */
    cwist_free(g_state.cached_json);
    g_state.cached_json = NULL;
    pthread_mutex_unlock(&g_state.mu);
}
