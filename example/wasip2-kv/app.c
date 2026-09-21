/**
 * @file app.c
 * @brief WASI 0.2 edge app with host-KV persistence (issue #201 Phase 1).
 *
 * The guest keeps cwist_db in memory. After every mutation it serializes the
 * SQLite image (cwist_db_serialize) and writes the blob to kv/cwist.db inside
 * the host-preopened directory — the wasmtime-appliance analogue of a Workers
 * KV put(). On boot it reads the blob back through cwist_db_open_memory() —
 * the KV get() side. Restarting the module therefore keeps the data.
 *
 * Routes:
 *   GET  /       service info + item count
 *   GET  /items  item rows as JSON
 *   POST /items  {"name": string, "qty": int} -> insert + persist blob
 */

#include <cwist/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef KV_PORT
#define KV_PORT 18100
#endif

/* Under the host-preopened dir (wasmtime --dir). On Workers the same blob
 * would live in a KV namespace instead of a file. */
#define KV_BLOB "kv/cwist.db"

static cwist_db *g_db;

/* ---- host-KV round trip -------------------------------------------------- */

/* KV put: serialize the in-memory db and store the blob on the host side. */
static int kv_put(void) {
    void *img = NULL;
    size_t n = 0;
    if (cwist_db_serialize(g_db, &img, &n).error.err_i16 != 0 || !img || n == 0)
        return -1;
    FILE *f = fopen(KV_BLOB, "wb");
    if (!f) {
        cwist_free(img);
        return -1;
    }
    size_t w = fwrite(img, 1, n, f);
    fclose(f);
    cwist_free(img);
    return w == n ? 0 : -1;
}

/* KV get: read the blob back and reopen it as an in-memory db. Falls back to
 * a fresh :memory: db when the blob is absent or unreadable. */
static cwist_db *kv_get_or_create(void) {
    FILE *f = fopen(KV_BLOB, "rb");
    if (f) {
        cwist_db *db = NULL;
        if (fseek(f, 0, SEEK_END) == 0) {
            long n = ftell(f);
            rewind(f);
            void *buf = n > 0 ? malloc((size_t)n) : NULL;
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n &&
                cwist_db_open_memory(&db, buf, (size_t)n, 0).error.err_i16 == 0 && db) {
                fclose(f);
                free(buf);
                return db;
            }
            free(buf);
        }
        fclose(f);
    }
    cwist_db *db = NULL;
    if (cwist_db_open(&db, ":memory:").error.err_i16 != 0 || !db)
        return NULL;
    cwist_db_exec(db,
                  "CREATE TABLE IF NOT EXISTS items ("
                  "id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER NOT NULL)");
    return db;
}

/* ---- handlers ------------------------------------------------------------ */

static void json_response(cwist_http_response *res, int status, const char *json) {
    res->status_code = status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json);
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *rows = NULL;
    int count = 0;
    if (g_db &&
        cwist_db_query(g_db, "SELECT id FROM items", &rows).error.err_i16 == 0 && rows) {
        count = cJSON_GetArraySize(rows);
        cJSON_Delete(rows);
    }
    char out[160];
    snprintf(out, sizeof(out),
             "{\"service\":\"cwist-wasip2-kv\",\"items\":%d,\"blob\":\"%s\"}", count, KV_BLOB);
    json_response(res, CWIST_HTTP_OK, out);
}

static void items_list_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *rows = NULL;
    if (!g_db ||
        cwist_db_query(g_db, "SELECT id, name, qty FROM items ORDER BY id", &rows)
                .error.err_i16 != 0 ||
        !rows) {
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"db query\"}");
        return;
    }
    char *out = cJSON_PrintUnformatted(rows); /* values are strings: exec-callback semantics */
    cJSON_Delete(rows);
    json_response(res, CWIST_HTTP_OK, out ? out : "[]");
    free(out);
}

/* Minimal ' -> '' escaping for the demo INSERT; production code should bind. */
static void sql_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        if (*p == '\'')
            dst[o++] = '\'';
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

static void items_create_handler(cwist_http_request *req, cwist_http_response *res) {
    if (!req->body || !req->body->data) {
        json_response(res, CWIST_HTTP_BAD_REQUEST, "{\"ok\":false,\"error\":\"empty body\"}");
        return;
    }
    cJSON *parsed = cJSON_Parse(req->body->data);
    const cJSON *name = parsed ? cJSON_GetObjectItem(parsed, "name") : NULL;
    const cJSON *qty = parsed ? cJSON_GetObjectItem(parsed, "qty") : NULL;
    if (!cJSON_IsString(name) || !name->valuestring || !cJSON_IsNumber(qty)) {
        cJSON_Delete(parsed);
        json_response(res, CWIST_HTTP_BAD_REQUEST,
                      "{\"ok\":false,\"error\":\"want {name:string, qty:number}\"}");
        return;
    }
    char esc[256], sql[384];
    sql_escape(esc, sizeof(esc), name->valuestring);
    snprintf(sql, sizeof(sql), "INSERT INTO items (name, qty) VALUES ('%s', %d)", esc,
             qty->valueint);
    cJSON_Delete(parsed);
    if (cwist_db_exec(g_db, sql).error.err_i16 != 0) {
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"db insert\"}");
        return;
    }
    if (kv_put() != 0) {
        json_response(res, CWIST_HTTP_INTERNAL_ERROR, "{\"ok\":false,\"error\":\"persist\"}");
        return;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%ld}",
             (long)sqlite3_last_insert_rowid(g_db->conn));
    json_response(res, CWIST_HTTP_CREATED, out);
}

int main(void) {
    g_db = kv_get_or_create();
    if (!g_db) {
        fprintf(stderr, "wasip2-kv: db init failed\n");
        return 1;
    }
    cwist_app *app = cwist_app_create();
    if (!app) {
        fprintf(stderr, "wasip2-kv: app_create failed\n");
        return 1;
    }
    cwist_app_get(app, "/", index_handler);
    cwist_app_get(app, "/items", items_list_handler);
    cwist_app_post(app, "/items", items_create_handler);
    printf("wasip2-kv: listening on %d (blob %s)\n", KV_PORT, KV_BLOB);
    fflush(stdout);
    return cwist_app_listen(app, KV_PORT);
}
