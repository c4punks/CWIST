/* test_wasm_stream.c - WASM boundary streaming (issue #93 Phase 3).
 *
 * The streaming entry points (cwist_app_dispatch_stream, cwist_stream_req_*)
 * are host-agnostic: they run natively exactly as under Emscripten, so the
 * boundary semantics are covered here in the standard `make test` suite and
 * the Emscripten smoke test only re-verifies them through JS glue.
 *
 * Covers:
 *   - dispatch_stream delivers head first, body in <= CWIST_STREAM_CHUNK slices
 *   - concatenated stream equals cwist_app_dispatch_memory output
 *   - sink abort -> -2; malformed request -> -1
 *   - incremental request: begin/feed/end, overflow and short-body errors
 *   - session continuity across app instances with a pinned secret
 */
#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- sink that accumulates into a growable buffer ------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t chunks;
    size_t max_chunk;
    size_t abort_after; /* chunk index (0-based) at which to abort; SIZE_MAX = never */
} sink_t;

static int sink_write(void *ctx, const char *data, size_t len) {
    sink_t *s = ctx;
    if (s->chunks == s->abort_after) return 1;
    if (s->len + len > s->cap) {
        size_t ncap = s->cap ? s->cap : 4096;
        while (ncap < s->len + len) ncap *= 2;
        char *nb = cwist_alloc(ncap);
        if (!nb) return 1;
        if (s->len) memcpy(nb, s->buf, s->len);
        cwist_free(s->buf);
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    if (len > s->max_chunk) s->max_chunk = len;
    s->chunks++;
    return 0;
}

static void sink_free(sink_t *s) {
    cwist_free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

/* --- handlers ------------------------------------------------------------ */

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-stream");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) {
        cwist_sstring_assign(res->body, req->body->data);
    } else {
        cwist_sstring_assign(res->body, "");
    }
}

static void big_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* 150 KiB body: forces multiple CWIST_STREAM_CHUNK (64 KiB) slices. */
    char block[4096];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (char)('a' + (i % 26));
    for (int i = 0; i < 38; i++) {
        size_t n = sizeof(block);
        if (i == 37) n = (150 * 1024) - 37 * sizeof(block); /* exact total */
        cwist_sstring_append_len(res->body, block, n);
    }
}

/* Session handler: reads "user" out of the session, or stores the
 * ?user= query value when present.  Commits the session so the response
 * carries the signed cookie. */
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
        const char *want = req->query_params ? cwist_query_map_get(req->query_params, "user") : NULL;
        if (want) {
            cwist_session_set(sess, "user", want);
            cwist_sstring_assign(res->body, "stored");
        } else {
            cwist_sstring_assign(res->body, "anonymous");
        }
    }
    cwist_session_commit(sess, res);
    /* No cwist_session_destroy here: the request owns the session handle
     * and cwist_http_request_destroy releases it. */
}

static cwist_app *make_app(const char *secret) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, secret) == 0);
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_get(app, "/big", big_handler);
    cwist_app_post(app, "/echo", echo_handler);
    cwist_app_get(app, "/sess", session_handler);
    return app;
}

/* Extract the session cookie value from a serialized response's
 * Set-Cookie header; writes "name=value" into out. Returns 0 on success. */
static int extract_session_cookie(const char *res, size_t res_len, const char *name,
                                  char *out, size_t out_cap) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\r\nSet-Cookie: %s=", name);
    const char *p = res;
    const char *end = res + res_len;
    size_t nlen = strlen(needle);
    while (p + nlen < end) {
        if (memcmp(p, needle, nlen) == 0) {
            const char *v = p + nlen;
            const char *e = v;
            while (e < end && *e != '\r' && *e != ';') e++;
            size_t vl = (size_t)(e - v);
            if (vl >= out_cap) return -1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

int main(void) {
    cwist_app *app = make_app("phase3-test-secret-pinned-0123456789abcdef");

    /* 1. Streamed response matches the buffered dispatcher exactly. */
    static const char req[] = "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n";
    size_t mem_len = 0;
    char *mem = NULL;
    assert(cwist_app_dispatch_memory(app, req, sizeof(req) - 1, &mem, &mem_len) == 0);
    assert(mem && mem_len > 0);

    sink_t sink = {.abort_after = (size_t)-1};
    assert(cwist_app_dispatch_stream(app, req, sizeof(req) - 1, sink_write, &sink) == 0);
    assert(sink.len == mem_len);
    assert(memcmp(sink.buf, mem, mem_len) == 0);
    assert(sink.chunks >= 2); /* head + at least one body chunk */
    assert(strncmp(sink.buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    printf("streamed response equals buffered response (%zu bytes, %zu chunks)\n", sink.len,
           sink.chunks);
    sink_free(&sink);
    cwist_free(mem);

    /* 2. Body sliced at CWIST_STREAM_CHUNK, order preserved. */
    static const char big_req[] = "GET /big HTTP/1.1\r\nHost: t\r\n\r\n";
    sink = (sink_t){.abort_after = (size_t)-1};
    assert(cwist_app_dispatch_stream(app, big_req, sizeof(big_req) - 1, sink_write, &sink) == 0);
    assert(sink.max_chunk <= CWIST_STREAM_CHUNK);
    assert(sink.len > 150 * 1024);
    assert(sink.chunks >= 1 + (150 * 1024 + CWIST_STREAM_CHUNK - 1) / CWIST_STREAM_CHUNK);
    assert(strncmp(sink.buf + sink.len - 4, "\r\n\r\n", 4) != 0 ||
           sink.len > 150 * 1024); /* body present after head */
    printf("big body streamed in %zu chunks (max slice %zu)\n", sink.chunks, sink.max_chunk);
    sink_free(&sink);

    /* 3. Sink abort mid-body -> -2. */
    sink = (sink_t){.abort_after = 1}; /* abort on first body chunk */
    int rc = cwist_app_dispatch_stream(app, big_req, sizeof(big_req) - 1, sink_write, &sink);
    assert(rc == -2);
    printf("sink abort returns -2\n");
    sink_free(&sink);

    /* 4. Malformed request -> -1, sink untouched. */
    sink = (sink_t){.abort_after = (size_t)-1};
    assert(cwist_app_dispatch_stream(app, "garbage", 7, sink_write, &sink) == -1);
    assert(sink.len == 0);
    sink_free(&sink);

    /* 5. Incremental request: feed the body in 3 chunks. */
    static const char post_head[] =
        "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\n";
    cwist_stream_req_t *sr = cwist_stream_req_begin(post_head, sizeof(post_head) - 1);
    assert(sr != NULL);
    assert(cwist_stream_req_feed(sr, "hel", 3) == 0);
    assert(cwist_stream_req_feed(sr, "lo-", 3) == 0);
    assert(cwist_stream_req_feed(sr, "world", 5) == 0);
    /* overflow past Content-Length is rejected */
    assert(cwist_stream_req_feed(sr, "!", 1) == -1);
    assert(cwist_stream_req_end(sr) == 0);

    sink = (sink_t){.abort_after = (size_t)-1};
    assert(cwist_stream_req_dispatch(sr, app, sink_write, &sink) == 0);
    assert(sink.len > 11);
    assert(memcmp(sink.buf + sink.len - 11, "hello-world", 11) == 0);
    printf("incremental request (3 feeds) echoed correctly\n");
    sink_free(&sink);

    /* 6. Short body at end() -> -1; handle stays owned by caller. */
    sr = cwist_stream_req_begin(post_head, sizeof(post_head) - 1);
    assert(sr != NULL);
    assert(cwist_stream_req_feed(sr, "short", 5) == 0);
    assert(cwist_stream_req_end(sr) == -1);
    sink = (sink_t){.abort_after = (size_t)-1};
    assert(cwist_stream_req_dispatch(sr, app, sink_write, &sink) == -1); /* still short */
    sink_free(&sink);

    /* 7. Malformed Content-Length -> begin fails. */
    static const char bad_head[] =
        "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: bananas\r\n\r\n";
    assert(cwist_stream_req_begin(bad_head, sizeof(bad_head) - 1) == NULL);
    printf("overflow/short/malformed incremental cases rejected\n");

    /* 8. Body beyond the eager reservation grows on feed and echoes back
     * byte-for-byte (issue #237 item 10 grow path). */
    const size_t big_len = ((size_t)1 << 20) + 64 * 1024; /* > CWIST_STREAM_REQ_EAGER_MAX */
    char big_head[128];
    int hl = snprintf(big_head, sizeof(big_head),
                      "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\n\r\n", big_len);
    assert(hl > 0 && (size_t)hl < sizeof(big_head));
    sr = cwist_stream_req_begin(big_head, (size_t)hl);
    assert(sr != NULL);
    char chunk[64 * 1024];
    for (size_t off = 0; off < big_len;) {
        size_t n = big_len - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        /* stay in 1..255: the echo handler copies through sstring_assign,
         * which measures with strlen, so a NUL would truncate the body */
        for (size_t i = 0; i < n; i++) chunk[i] = (char)(1u + ((off + i) * 31u + 7u) % 255u);
        assert(cwist_stream_req_feed(sr, chunk, n) == 0);
        off += n;
    }
    assert(cwist_stream_req_end(sr) == 0);
    sink = (sink_t){.abort_after = (size_t)-1};
    assert(cwist_stream_req_dispatch(sr, app, sink_write, &sink) == 0);
    assert(sink.len > big_len);
    for (size_t i = 0; i < big_len; i++) {
        assert(sink.buf[sink.len - big_len + i] == (char)(1u + (i * 31u + 7u) % 255u));
    }
    printf("body beyond eager reservation grows and echoes (%zu bytes)\n", big_len);
    sink_free(&sink);

    /* 9. Session continuity across instances with a pinned secret. */
    static const char sess_set[] = "GET /sess?user=alice HTTP/1.1\r\nHost: t\r\n\r\n";
    size_t r1_len = 0;
    char *r1 = NULL;
    assert(cwist_app_dispatch_memory(app, sess_set, sizeof(sess_set) - 1, &r1, &r1_len) == 0);
    char cookie[4096];
    assert(extract_session_cookie(r1, r1_len, "cwist_session", cookie, sizeof(cookie)) == 0);
    cwist_free(r1);

    /* Second request on a *different* app instance, same secret: the
     * session cookie must verify and carry "user=alice" through. */
    cwist_app *app2 = make_app("phase3-test-secret-pinned-0123456789abcdef");
    char sess_get[4400];
    int gl = snprintf(sess_get, sizeof(sess_get),
                      "GET /sess HTTP/1.1\r\nHost: t\r\nCookie: cwist_session=%s\r\n\r\n", cookie);
    assert(gl > 0 && (size_t)gl < sizeof(sess_get));
    size_t r2_len = 0;
    char *r2 = NULL;
    assert(cwist_app_dispatch_memory(app2, sess_get, (size_t)gl, &r2, &r2_len) == 0);
    assert(r2_len >= 5 && memcmp(r2 + r2_len - 5, "alice", 5) == 0);
    cwist_free(r2);
    printf("session cookie verifies across app instances with the pinned secret\n");

    /* Different secret: the same cookie must NOT verify. */
    cwist_app *app3 = make_app("a-different-secret-should-fail-verification");
    size_t r3_len = 0;
    char *r3 = NULL;
    assert(cwist_app_dispatch_memory(app3, sess_get, (size_t)gl, &r3, &r3_len) == 0);
    assert(r3_len >= 9 && memcmp(r3 + r3_len - 9, "anonymous", 9) == 0);
    cwist_free(r3);
    cwist_app_destroy(app3);
    cwist_app_destroy(app2);
    cwist_app_destroy(app);
    printf("session rejected under a different secret\n");

    printf("All WASM streaming/session boundary tests passed.\n");
    return 0;
}
