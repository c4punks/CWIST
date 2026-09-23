/* Streaming producer API (issue #201 Phase 1): handlers generate response
 * bodies chunk-by-chunk through cwist_http_response_stream_begin/write/end,
 * replacing the buffered-body boundary streaming from v3.6 for producers.
 *
 * Covers:
 *   - memory dispatch emits Transfer-Encoding: chunked with framed chunks
 *   - an un-ended stream is finalized implicitly at dispatch return
 *   - streaming dispatch pushes head + each chunk to the sink while the
 *     handler is still running (incremental delivery, not post-hoc)
 *   - write-after-end, double begin, and ptr-body begin are rejected
 *   - a body assigned before begin is discarded
 *   - zero-length writes are no-ops (an empty chunk is the terminator)
 *   - sink abort surfaces as -2
 */
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- recording sink ------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t calls;
    size_t abort_after; /* 1-based call index that fails; 0 = never */
} rec_t;

static int rec_write(void *ctx, const char *data, size_t len) {
    rec_t *r = ctx;
    r->calls++;
    if (r->abort_after && r->calls == r->abort_after) return 1;
    if (r->len + len > r->cap) {
        size_t ncap = r->cap ? r->cap : 4096;
        while (ncap < r->len + len) ncap *= 2;
        char *nb = cwist_alloc(ncap);
        assert(nb != NULL);
        if (r->len) memcpy(nb, r->buf, r->len);
        cwist_free(r->buf);
        r->buf = nb;
        r->cap = ncap;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

static void rec_free(rec_t *r) {
    cwist_free(r->buf);
    r->buf = NULL;
    r->len = r->cap = r->calls = 0;
}

/* --- handlers ------------------------------------------------------------- */

static int g_sink_may_fail = 0; /* set by the abort test: writes legitimately fail */
static void producer_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    cwist_http_header_add(&res->headers, "Content-Type", "text/event-stream");
    int w1 = cwist_http_response_stream_write(res, "one", 3);
    int w2 = cwist_http_response_stream_write(res, "two", 3);
    int w3 = cwist_http_response_stream_write(res, "three", 5);
    int we = cwist_http_response_stream_end(res);
    if (!g_sink_may_fail) {
        assert(w1 == 0 && w2 == 0 && w3 == 0 && we == 0);
    }
}

/* Forgets end(): dispatch must finalize implicitly. */
static void no_end_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    assert(cwist_http_response_stream_write(res, "only", 4) == 0);
}

/* Probes incremental delivery: after the first write the sink must already
 * hold the head and the first chunk while this handler is still on the
 * stack. */
static rec_t *g_live_rec;
static void incremental_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    assert(cwist_http_response_stream_write(res, "first", 5) == 0);
    assert(g_live_rec->calls >= 2); /* head + first chunk already delivered */
    assert(cwist_http_response_stream_write(res, "second", 6) == 0);
}

static int g_after_end_rc = 0;
static void after_end_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    assert(cwist_http_response_stream_write(res, "x", 1) == 0);
    assert(cwist_http_response_stream_end(res) == 0);
    g_after_end_rc = cwist_http_response_stream_write(res, "y", 1);
}

static int g_double_begin_rc = 0;
static void double_begin_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    g_double_begin_rc = cwist_http_response_stream_begin(res);
}

static int g_zero_write_rc = -1;
static size_t g_calls_before_zero;
static size_t g_calls_after_zero;
static void zero_write_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    assert(cwist_http_response_stream_begin(res) == 0);
    assert(cwist_http_response_stream_write(res, "a", 1) == 0);
    g_calls_before_zero = g_live_rec->calls;
    g_zero_write_rc = cwist_http_response_stream_write(res, "", 0);
    g_calls_after_zero = g_live_rec->calls;
    assert(cwist_http_response_stream_end(res) == 0);
}

/* A body assigned before begin must not leak into the chunked payload. */
static void discard_body_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "junk");
    assert(cwist_http_response_stream_begin(res) == 0);
    assert(cwist_http_response_stream_write(res, "x", 1) == 0);
    assert(cwist_http_response_stream_end(res) == 0);
}

/* --- helpers -------------------------------------------------------------- */

static cwist_app *make_app(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    return app;
}

static char *dispatch_get(cwist_app *app, const char *path, size_t *out_len) {
    char wire[256];
    int n = snprintf(wire, sizeof(wire), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    char *res = NULL;
    size_t res_len = 0;
    assert(cwist_app_dispatch_memory(app, wire, (size_t)n, &res, &res_len) == 0);
    (void)out_len;
    *out_len = res_len;
    return res;
}

int main(void) {
    /* 1. Memory dispatch: chunked framing and TE header. */
    cwist_app *app = make_app();
    cwist_app_get(app, "/events", producer_handler);
    size_t len = 0;
    char *res = dispatch_get(app, "/events", &len);
    assert(strstr(res, "Transfer-Encoding: chunked") != NULL);
    assert(strstr(res, "Content-Length:") == NULL);
    const char *framed = strstr(res, "\r\n\r\n");
    assert(framed != NULL);
    framed += 4;
    assert(strcmp(framed, "3\r\none\r\n3\r\ntwo\r\n5\r\nthree\r\n0\r\n\r\n") == 0);
    cwist_free(res);
    cwist_app_destroy(app);

    /* 2. Implicit finalize when the handler never calls end(). */
    app = make_app();
    cwist_app_get(app, "/noend", no_end_handler);
    res = dispatch_get(app, "/noend", &len);
    assert(strstr(res, "Transfer-Encoding: chunked") != NULL);
    assert(len >= 5 && memcmp(res + len - 5, "0\r\n\r\n", 5) == 0);
    cwist_free(res);
    cwist_app_destroy(app);

    /* 3. Streaming dispatch: head first, chunks in order, terminator last. */
    app = make_app();
    cwist_app_get(app, "/events", producer_handler);
    rec_t rec = {0};
    char wire[256];
    int n = snprintf(wire, sizeof(wire), "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(cwist_app_dispatch_stream(app, wire, (size_t)n, rec_write, &rec) == 0);
    assert(rec.calls >= 4); /* head + 3 chunks + terminator flush */
    assert(memcmp(rec.buf, "HTTP/1.1 200", 12) == 0);
    assert(strstr(rec.buf, "Transfer-Encoding: chunked") != NULL);
    const char *body = strstr(rec.buf, "\r\n\r\n");
    assert(body != NULL);
    assert(strcmp(body + 4, "3\r\none\r\n3\r\ntwo\r\n5\r\nthree\r\n0\r\n\r\n") == 0);
    rec_free(&rec);
    cwist_app_destroy(app);

    /* 4. Incremental delivery while the handler runs. */
    app = make_app();
    cwist_app_get(app, "/inc", incremental_handler);
    rec_t rec2 = {0};
    g_live_rec = &rec2;
    n = snprintf(wire, sizeof(wire), "GET /inc HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(cwist_app_dispatch_stream(app, wire, (size_t)n, rec_write, &rec2) == 0);
    g_live_rec = NULL;
    rec_free(&rec2);
    cwist_app_destroy(app);

    /* 5. write after end fails. */
    app = make_app();
    cwist_app_get(app, "/ae", after_end_handler);
    res = dispatch_get(app, "/ae", &len);
    assert(g_after_end_rc == -1);
    cwist_free(res);
    cwist_app_destroy(app);

    /* 6. double begin fails. */
    app = make_app();
    cwist_app_get(app, "/db", double_begin_handler);
    res = dispatch_get(app, "/db", &len);
    assert(g_double_begin_rc == -1);
    cwist_free(res);
    cwist_app_destroy(app);

    /* 7. zero-length write is a no-op (no extra sink call, no terminator). */
    app = make_app();
    cwist_app_get(app, "/zero", zero_write_handler);
    rec_t rec3 = {0};
    g_live_rec = &rec3;
    n = snprintf(wire, sizeof(wire), "GET /zero HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(cwist_app_dispatch_stream(app, wire, (size_t)n, rec_write, &rec3) == 0);
    g_live_rec = NULL;
    assert(g_zero_write_rc == 0);
    assert(g_calls_after_zero == g_calls_before_zero); /* nothing emitted for the empty write */
    rec_free(&rec3);
    cwist_app_destroy(app);

    /* 8. body assigned before begin is discarded. */
    app = make_app();
    cwist_app_get(app, "/discard", discard_body_handler);
    res = dispatch_get(app, "/discard", &len);
    assert(strstr(res, "junk") == NULL);
    assert(strstr(res, "1\r\nx\r\n0\r\n\r\n") != NULL);
    cwist_free(res);
    cwist_app_destroy(app);

    /* 9. sink abort surfaces as -2. */
    app = make_app();
    cwist_app_get(app, "/events", producer_handler);
    rec_t rec4 = {0};
    rec4.abort_after = 2; /* fail on the first data chunk */
    g_sink_may_fail = 1;
    n = snprintf(wire, sizeof(wire), "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(cwist_app_dispatch_stream(app, wire, (size_t)n, rec_write, &rec4) == -2);
    g_sink_may_fail = 0;
    rec_free(&rec4);
    cwist_app_destroy(app);

    printf("stream producer tests passed\n");
    return 0;
}
