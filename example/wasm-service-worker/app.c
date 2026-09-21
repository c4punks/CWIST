/**
 * @file app.c
 * @brief CWIST app for the WASM Service Worker demo (issue #93 Phase 4).
 *
 * One C file exercising the whole WASM-relevant stack: routing, zod
 * validation, template rendering, cwist_db (SQLite :memory:), and signed-cookie
 * sessions. The host (sw.js in a browser, smoke.js under node) feeds in raw
 * HTTP/1.1 request bytes through the CWIST_WASM_DEFINE_ENTRY entry points and
 * receives the serialized response; no sockets are involved.
 *
 * Routes:
 *   GET  /             template-rendered page (session visit counter + items)
 *   POST /items        JSON body, zod-validated, inserted into cwist_db
 *   GET  /items        item list from cwist_db as JSON
 *   GET  /items/image  serialized SQLite image (cwist_db_serialize), the blob
 *                      an edge host would persist outside the WASM instance
 *   anything else      router's default 404
 *
 * The session signing secret is pinned by the host via _cwist_wasm_use_session
 * (sw.js injects one before the first dispatch; smoke.js does the same through
 * the cwist-wasm wrapper) so signed cookies verify across module restarts.
 */

#include <cwist/sys/app/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/template/template.h>
#include <cwist/core/utils/zod.h>
#include <cwist/net/http/session.h>
#include <cwist/wasm/wasm_entry.h>
#include <stdio.h>
#include <string.h>

static cwist_app *g_app;
static cwist_db *g_db;

/* zod schema for POST /items: strict, rejects unknown shapes implicitly by
 * requiring both fields with the declared types. "qty" also answers to the
 * aliases "quantity"/"count" after healing, but this endpoint validates
 * strictly and takes no aliases. */
static const cwist_schema_field_t s_item_fields[] = {
    {"name", {NULL}, CWIST_FIELD_STRING, true},
    {"qty", {NULL}, CWIST_FIELD_INT, true},
};
static const cwist_schema_t s_item_schema = {s_item_fields, 2};

/* Rendered for GET /. The form POSTs JSON to /items; the item list below is
 * populated from cwist_db through a {% for %} loop. User-controlled values go
 * through the | escape filter. */
static const char s_page_template[] =
    "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>CWIST in a Service Worker</title>\n"
    "<style>\n"
    "body{font-family:system-ui,sans-serif;max-width:42em;margin:2em auto;padding:0 1em;"
    "color:#222}\n"
    "h1{border-bottom:2px solid #46f;border-bottom:2px solid #46f;padding-bottom:.2em}\n"
    "input,button{font:inherit;padding:.35em .6em;margin:.15em 0}\n"
    "pre{background:#f4f4f4;padding:.6em;white-space:pre-wrap}\n"
    "li{padding:.1em 0}\n"
    "</style>\n</head>\n<body>\n"
    "<h1>CWIST inside a Service Worker</h1>\n"
    "<p>This page was routed, validated, rendered, and served by CWIST running in "
    "WebAssembly. A Service Worker intercepted the fetch, dispatched it into the "
    "WASM module, and carried the session cookie itself.</p>\n"
    "<p>Session visits: <strong>{{ visits }}</strong> &middot; items in cwist_db: "
    "<strong>{{ item_count }}</strong></p>\n"
    "<h2>Add an item <small>(POST /items, zod-validated)</small></h2>\n"
    "<form id=\"add-form\">\n"
    "<input name=\"name\" placeholder=\"name\" required>\n"
    "<input name=\"qty\" type=\"number\" min=\"1\" step=\"1\" placeholder=\"qty\" required>\n"
    "<button type=\"submit\">Add</button>\n</form>\n"
    "<pre id=\"add-result\"></pre>\n"
    "<h2>Items <small>(GET /items out of cwist_db)</small></h2>\n"
    "{% if items %}\n<ul>\n"
    "{% for item in items %}<li>#{{ item.id | escape }} &mdash; {{ item.name | escape }} "
    "&times; {{ item.qty | escape }}</li>\n{% endfor %}\n"
    "</ul>\n"
    "{% else %}\n<p><em>No items yet &mdash; add one above.</em></p>\n"
    "{% endif %}\n"
    "<script>\n"
    "document.getElementById('add-form').addEventListener('submit', async (ev) => {\n"
    "  ev.preventDefault();\n"
    "  const fd = new FormData(ev.target);\n"
    "  const out = document.getElementById('add-result');\n"
    "  const res = await fetch('/items', {\n"
    "    method: 'POST',\n"
    "    headers: {'Content-Type': 'application/json'},\n"
    "    body: JSON.stringify({name: fd.get('name'), qty: Number(fd.get('qty'))}),\n"
    "  });\n"
    "  out.textContent = res.status + ' ' + (await res.text());\n"
    "  if (res.ok) setTimeout(() => location.reload(), 400);\n"
    "});\n"
    "</script>\n"
    "</body>\n</html>\n";

/* Minimal ' -> '' escaping for the demo INSERT. Production code should use
 * bound parameters; cwist_db exposes exec-style entry points only. */
static void sql_escape(char *dst, size_t dst_cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < dst_cap; p++) {
        if (*p == '\'') {
            dst[o++] = '\'';
            dst[o++] = '\'';
        } else
            dst[o++] = *p;
    }
    dst[o] = '\0';
}

static void json_response(cwist_http_response *res, int status, const char *json) {
    res->status_code = status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json);
}

/* 400 with the zod error list as JSON. */
static void zod_error_response(cwist_http_response *res, const cwist_zod_result_t *zr) {
    cJSON *err = cJSON_CreateObject();
    if (!err) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cJSON_AddFalseToObject(err, "ok");
    cJSON *errors = cJSON_AddArrayToObject(err, "errors");
    if (errors) {
        for (int i = 0; i < zr->error_count && i < CWIST_ZOD_MAX_ERRORS; i++) {
            cJSON *e = cJSON_CreateObject();
            if (!e) break;
            cJSON_AddStringToObject(e, "field", zr->errors[i].field);
            cJSON_AddStringToObject(e, "message", zr->errors[i].message);
            cJSON_AddItemToArray(errors, e);
        }
    }
    char *out = cJSON_PrintUnformatted(err);
    cJSON_Delete(err);
    json_response(res, CWIST_HTTP_BAD_REQUEST, out ? out : "{\"ok\":false}");
    free(out);
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    /* Sessions are client-side signed cookies: bump the counter, commit() the
     * session, and the response carries the renewed Set-Cookie. */
    long visits = 1;
    cwist_session_t *sess = cwist_session_start(req->app, req, res);
    if (sess) {
        const char *prev = cwist_session_get(sess, "visits");
        if (prev && *prev) visits = strtol(prev, NULL, 10) + 1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld", visits);
        cwist_session_set(sess, "visits", buf);
        cwist_session_commit(sess, res);
    }

    cJSON *ctx = cJSON_CreateObject();
    if (!ctx) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cJSON_AddNumberToObject(ctx, "visits", (double)visits);

    cJSON *items = NULL;
    if (g_db &&
        cwist_db_query(g_db, "SELECT id, name, qty FROM items ORDER BY id", &items).error.err_i16 ==
            0 &&
        items) {
        cJSON_AddItemToObject(ctx, "items", items);
        cJSON_AddNumberToObject(ctx, "item_count", cJSON_GetArraySize(items));
    } else {
        cJSON_AddArrayToObject(ctx, "items");
        cJSON_AddNumberToObject(ctx, "item_count", 0);
    }

    cwist_sstring *html = cwist_template_render(s_page_template, ctx);
    cJSON_Delete(ctx);
    if (!html) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
}

static void items_create_handler(cwist_http_request *req, cwist_http_response *res) {
    if (!req->body || !req->body->data || !g_db) {
        json_response(res, CWIST_HTTP_BAD_REQUEST, "{\"ok\":false,\"error\":\"empty body\"}");
        return;
    }

    cJSON *parsed = NULL;
    cwist_zod_result_t zr = cwist_zod_parse(req->body->data, &s_item_schema, &parsed);
    if (!zr.valid || !parsed) {
        zod_error_response(res, &zr);
        return;
    }

    const cJSON *name = cJSON_GetObjectItem(parsed, "name");
    const cJSON *qty = cJSON_GetObjectItem(parsed, "qty");
    char escaped[256];
    sql_escape(escaped, sizeof(escaped), name->valuestring);

    char sql[384];
    snprintf(sql, sizeof(sql), "INSERT INTO items (name, qty) VALUES ('%s', %d)", escaped,
             qty->valueint);
    if (cwist_db_exec(g_db, sql).error.err_i16 != 0) {
        cJSON_Delete(parsed);
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"db insert\"}");
        return;
    }
    long id = (long)sqlite3_last_insert_rowid(g_db->conn);
    cJSON_Delete(parsed);

    char out[128];
    snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%ld}", id);
    json_response(res, CWIST_HTTP_CREATED, out);
}

static void items_list_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    if (!g_db) {
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"no db\"}");
        return;
    }
    cJSON *rows = NULL;
    if (cwist_db_query(g_db, "SELECT id, name, qty FROM items ORDER BY id", &rows).error.err_i16 !=
            0 ||
        !rows) {
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"db query\"}");
        return;
    }
    /* Values come back as strings (sqlite exec-callback semantics). */
    char *out = cJSON_PrintUnformatted(rows);
    cJSON_Delete(rows);
    json_response(res, CWIST_HTTP_OK, out ? out : "[]");
    free(out);
}

static void items_image_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    if (!g_db) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    /* cwist_db_serialize: the blob-shaped database an edge host would persist
     * outside the WASM instance and hand back via cwist_db_open_memory(). */
    void *image = NULL;
    size_t image_len = 0;
    if (cwist_db_serialize(g_db, &image, &image_len).error.err_i16 != 0 || !image ||
        image_len == 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
    cwist_sstring_append_len(res->body, (const char *)image, image_len);
    cwist_free(image);
}

/* File scope: the macro defines functions; g_app and g_db are populated in
 * main() before any dispatch runs. */
CWIST_WASM_DEFINE_ENTRY(g_app)

int main(void) {
    g_app = cwist_app_create();
    if (!g_app) return 1;

    if (cwist_db_open(&g_db, ":memory:").error.err_i16 != 0 || !g_db) return 2;
    if (cwist_db_exec(g_db, "CREATE TABLE IF NOT EXISTS items ("
                            "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  name TEXT NOT NULL,"
                            "  qty  INTEGER NOT NULL"
                            ")")
            .error.err_i16 != 0)
        return 3;

    cwist_app_get(g_app, "/", index_handler);
    cwist_app_post(g_app, "/items", items_create_handler);
    cwist_app_get(g_app, "/items", items_list_handler);
    cwist_app_get(g_app, "/items/image", items_image_handler);

    /* The session secret is pinned by the host (_cwist_wasm_use_session) so
     * cookies survive module restarts; see docs/api/wasm.md. */
    return 0;
}
