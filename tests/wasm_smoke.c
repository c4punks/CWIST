/* WASM smoke test for the in-memory dispatcher (not wired into CI — CI has
 * no Emscripten).  Manual build & run:
 *
 *   make wasm EMCC=/workspace/emsdk/upstream/emscripten/emcc \
 *             EMAR=/workspace/emsdk/upstream/emscripten/emar
 *   /workspace/emsdk/upstream/emscripten/emcc -std=c17 -O2 -I./include -I./lib \
 *       -o wasm_smoke.js tests/wasm_smoke.c libcwist_wasm.a
 *   node wasm_smoke.js
 */
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/db/sql.h>
#include <cwist/wasm/typedarray.h>
#include <stdio.h>
#include <string.h>

static const int32_t g_samples[] = {10, -20, 30, 40};
CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)
CWIST_WASM_INSTALL_VIEWS()

/* JS reads the response through a HEAPU8 view and the exposed struct array
 * through cwistView.i32 — any mismatch throws and node exits non-zero.
 * clang-format off: this brace block is a JS function body, not C. */
EM_JS(void, js_verify, (const char *res_ptr, int res_len), {
    // clang-format off
    const bytes = Module.cwistView.u8(res_ptr, res_len);
    const text = new TextDecoder().decode(bytes);
    if (!text.startsWith("HTTP/1.1 200 OK\r\n") || !text.endsWith("hello-wasm")) {
        throw new Error("bad response via TypedArray: " + JSON.stringify(text));
    }
    const samples = Module.cwistView.i32(_samples_ptr(), _samples_len());
    const total = Array.from(samples).reduce((a, b) => a + b, 0);
    if (total !== 60 || samples.length !== 4) {
        throw new Error("bad i32 view: " + Array.from(samples).join(","));
    }
    console.log("wasm_smoke: JS TypedArray views verified (" + res_len +
                " response bytes, samples sum " + total + ")");
    // clang-format on
});

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-wasm");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_app_get(app, "/hello", hello_handler);
    cwist_wasm_install_views();

    static const char req[] = "GET /hello HTTP/1.1\r\nHost: wasm\r\n\r\n";
    size_t res_len = 0;
    const char *res_buf = cwist_wasm_dispatch_memory(app, req, sizeof(req) - 1, &res_len);
    if (!res_buf) {
        fprintf(stderr, "wasm_smoke: dispatch failed\n");
        cwist_app_destroy(app);
        return 2;
    }

    int ok = strncmp(res_buf, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
             strstr(res_buf, "Content-Type: text/plain") != NULL && res_len >= 10 &&
             memcmp(res_buf + res_len - 10, "hello-wasm", 10) == 0;
    if (!ok) {
        fprintf(stderr, "wasm_smoke: bad response:\n%.*s\n", (int)res_len, res_buf);
        cwist_wasm_free((void *)res_buf);
        cwist_app_destroy(app);
        return 3;
    }

    /* Zero-copy handoff: JS reads the same heap bytes as a Uint8Array. */
    js_verify(res_buf, (int)res_len);
    cwist_wasm_free((void *)res_buf);
    cwist_app_destroy(app);

    /* cwist_db round trip: build a database, serialize it to an image blob,
     * reopen the image read-only through cwist_db_open_memory() (the
     * WASM/edge entry point), and read the row back. */
    cwist_db *db = NULL;
    if (cwist_db_open(&db, ":memory:").error.err_i16 != 0 || !db) {
        fprintf(stderr, "wasm_smoke: db open failed\n");
        return 4;
    }
    if (cwist_db_exec(db, "CREATE TABLE kv (k TEXT PRIMARY KEY, v INTEGER)").error.err_i16 != 0 ||
        cwist_db_exec(db, "INSERT INTO kv (k, v) VALUES ('wasm', 42)").error.err_i16 != 0) {
        fprintf(stderr, "wasm_smoke: db setup failed\n");
        cwist_db_close(db);
        return 5;
    }
    void *image = NULL;
    size_t image_len = 0;
    if (cwist_db_serialize(db, &image, &image_len).error.err_i16 != 0 || !image || image_len == 0) {
        fprintf(stderr, "wasm_smoke: db serialize failed\n");
        cwist_db_close(db);
        return 6;
    }
    cwist_db_close(db);

    cwist_db *reopened = NULL;
    if (cwist_db_open_memory(&reopened, image, image_len, 1).error.err_i16 != 0 || !reopened) {
        fprintf(stderr, "wasm_smoke: db open_memory failed\n");
        cwist_free(image);
        return 7;
    }
    cJSON *rows = NULL;
    if (cwist_db_query(reopened, "SELECT v FROM kv WHERE k = 'wasm'", &rows).error.err_i16 != 0 ||
        !rows || cJSON_GetArraySize(rows) != 1) {
        fprintf(stderr, "wasm_smoke: db query via image failed\n");
        if (rows) cJSON_Delete(rows);
        cwist_db_close(reopened);
        cwist_free(image);
        return 8;
    }
    /* cwist_db_query returns values as strings (sqlite3_exec callback). */
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "v");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, "42") != 0) {
        fprintf(stderr, "wasm_smoke: db value mismatch via image\n");
        cJSON_Delete(rows);
        cwist_db_close(reopened);
        cwist_free(image);
        return 9;
    }
    cJSON_Delete(rows);
    cwist_db_close(reopened);
    cwist_free(image);
    printf("wasm_smoke: db serialize/open_memory round trip OK (%zu byte image)\n", image_len);

    printf("wasm_smoke: OK\n");
    return 0;
}
