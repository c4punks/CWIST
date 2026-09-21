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
#include <cwist/net/http/session.h>
#include <cwist/wasm/typedarray.h>
#include <stdio.h>
#include <string.h>

static const int32_t g_samples[] = {10, -20, 30, 40};
CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)
CWIST_WASM_INSTALL_VIEWS()

/* Streaming boundary (Phase 3): accumulate response chunks on the JS side
 * through the same hook shape wasm_entry.h uses (Module.cwistStreamChunk). */
EM_JS(void, js_stream_reset, (void), {
    // clang-format off
    globalThis.__streamChunks = [];
    Module.cwistStreamChunk = function (ptr, len) {
        globalThis.__streamChunks.push(new Uint8Array(HEAPU8.buffer, ptr, len));
        return 0;
    };
    // clang-format on
});

EM_JS(int, js_stream_total, (void), {
    // clang-format off
    return globalThis.__streamChunks.reduce(function (n, c) { return n + c.length; }, 0);
    // clang-format on
});

EM_JS(int, js_chunk_bridge, (const char *data, size_t len), {
    // clang-format off
    if (typeof Module.cwistStreamChunk === "function") {
        return Module.cwistStreamChunk(data, len) ? 1 : 0;
    }
    return 0;
    // clang-format on
});

static int js_sink(void *ctx, const char *data, size_t len) {
    (void)ctx;
    /* Feed each slice through the JS hook so the smoke test exercises the
     * same chunk path a Service Worker host would use. */
    return js_chunk_bridge(data, len);
}

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

static void big_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char block[4096];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (char)('a' + (i % 26));
    for (int i = 0; i < 38; i++) {
        size_t n = sizeof(block);
        if (i == 37) n = (150 * 1024) - 37 * sizeof(block);
        cwist_sstring_append_len(res->body, block, n);
    }
}

static void session_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *sess = cwist_session_start(req->app, req, res);
    if (!sess) {
        res->status_code = 500;
        return;
    }
    const char *existing = cwist_session_get(sess, "user");
    if (existing && *existing) {
        cwist_sstring_assign(res->body, existing);
    } else {
        const char *want =
            req->query_params ? cwist_query_map_get(req->query_params, "user") : NULL;
        cwist_session_set(sess, "user", want ? want : "anonymous");
        cwist_sstring_assign(res->body, "stored");
    }
    cwist_session_commit(sess, res);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_get(app, "/big", big_handler);
    cwist_app_get(app, "/sess", session_handler);
    /* Sessions in WASM: pin the secret so cookies verify across WASM
     * instances (the host may drop and recreate the module at any time). */
    if (cwist_app_use_session(app, "wasm-smoke-pinned-secret-0123456789abcdef") != 0) return 1;
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

    /* Streaming boundary: /big (150 KiB) through the JS chunk hook must
     * total exactly the buffered dispatcher's output length. */
    {
        static const char big_req[] = "GET /big HTTP/1.1\r\nHost: wasm\r\n\r\n";
        size_t mem_len = 0;
        char *mem = NULL;
        if (cwist_app_dispatch_memory(app, big_req, sizeof(big_req) - 1, &mem, &mem_len) != 0 ||
            !mem) {
            fprintf(stderr, "wasm_smoke: buffered /big dispatch failed\n");
            return 10;
        }

        js_stream_reset();
        int src = cwist_app_dispatch_stream(app, big_req, sizeof(big_req) - 1, js_sink, NULL);
        int js_total = js_stream_total();
        if (src != 0 || (size_t)js_total != mem_len) {
            fprintf(stderr, "wasm_smoke: stream mismatch rc=%d js=%d mem=%zu\n", src, js_total,
                    mem_len);
            cwist_free(mem);
            return 11;
        }
        cwist_free(mem);
        printf("wasm_smoke: streaming dispatch matches buffered (%d bytes via JS chunks)\n",
               js_total);
    }

    /* Session persistence model: set on one "instance" (app), read back on
     * a fresh app with the same pinned secret - the signed cookie carries
     * the state, so instance lifetime is irrelevant. */
    {
        static const char set_req[] = "GET /sess?user=alice HTTP/1.1\r\nHost: wasm\r\n\r\n";
        size_t r1_len = 0;
        char *r1 = NULL;
        if (cwist_app_dispatch_memory(app, set_req, sizeof(set_req) - 1, &r1, &r1_len) != 0 ||
            !r1) {
            fprintf(stderr, "wasm_smoke: session set dispatch failed\n");
            return 12;
        }
        const char *sc = strstr(r1, "\r\nSet-Cookie: cwist_session=");
        if (!sc) {
            fprintf(stderr, "wasm_smoke: no session cookie in response\n");
            cwist_free(r1);
            return 13;
        }
        sc += strlen("\r\nSet-Cookie: cwist_session=");
        const char *sc_end = sc;
        while (*sc_end && *sc_end != '\r' && *sc_end != ';') sc_end++;
        char cookie[4096];
        size_t cl = (size_t)(sc_end - sc);
        if (cl >= sizeof(cookie)) {
            cwist_free(r1);
            return 14;
        }
        memcpy(cookie, sc, cl);
        cookie[cl] = '\0';
        cwist_free(r1);

        cwist_app *app2 = cwist_app_create();
        if (!app2) return 15;
        cwist_app_get(app2, "/sess", session_handler);
        if (cwist_app_use_session(app2, "wasm-smoke-pinned-secret-0123456789abcdef") != 0)
            return 16;
        char get_req[4400];
        int gl = snprintf(get_req, sizeof(get_req),
                          "GET /sess HTTP/1.1\r\nHost: wasm\r\nCookie: cwist_session=%s\r\n\r\n",
                          cookie);
        if (gl <= 0 || (size_t)gl >= sizeof(get_req)) return 17;
        size_t r2_len = 0;
        char *r2 = NULL;
        if (cwist_app_dispatch_memory(app2, get_req, (size_t)gl, &r2, &r2_len) != 0 || !r2) {
            fprintf(stderr, "wasm_smoke: session get dispatch failed\n");
            cwist_app_destroy(app2);
            return 18;
        }
        if (r2_len < 5 || memcmp(r2 + r2_len - 5, "alice", 5) != 0) {
            fprintf(stderr, "wasm_smoke: session did not survive instance swap\n");
            cwist_free(r2);
            cwist_app_destroy(app2);
            return 19;
        }
        cwist_free(r2);
        cwist_app_destroy(app2);
        printf("wasm_smoke: session cookie verifies across WASM instances (pinned secret)\n");
    }

    printf("wasm_smoke: OK\n");
    cwist_app_destroy(app);
    return 0;
}
