/**
 * @file test_builtin_mw_deferred.c
 * @brief Built-in middleware leaves a deferred response alone after next().
 *
 * Once a handler calls cwist_async_defer() the response belongs to its
 * cwist_async completion, which may write it on another thread at any time
 * (cwist_async_respond(), the timeout job). Here the handler defers and then
 * writes the response itself, standing in for a completion that has already
 * answered by the time the middleware resumes: the compression and access-log
 * middleware must neither change nor read it.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Always evaluated, unlike assert(). */
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                        \
        }                                                                   \
    } while (0)

#define BODY_LEN 4095

static bool g_defer = false;
static cwist_async *g_async = NULL;

static void handler(cwist_http_request *req, cwist_http_response *res) {
    if (g_defer) {
        g_async = cwist_async_defer(req, res);
        REQUIRE(g_async);
    }
    char body[BODY_LEN + 1];
    for (size_t i = 0; i < BODY_LEN; i++) body[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    body[BODY_LEN] = '\0';
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, body);
}

static cwist_http_request *make_request(void) {
    cwist_http_request *req = cwist_http_request_create();
    REQUIRE(req);
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/x");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "gzip");
    cwist_http_header_add(&req->headers, "X-Request-Id", "rid-from-request");
    return req;
}

/* Runs @p mw once around handler(), deferred or not. */
static void run(cwist_middleware_func mw, bool defer, cwist_http_request *req,
                cwist_http_response *res) {
    g_defer = defer;
    g_async = NULL;
    mw(req, res, handler);
    REQUIRE(res->deferred == defer);
    if (g_async) {
        /* Nothing completes it here: drop the completion's reference. */
        cwist_async_release(g_async);
        g_async = NULL;
    }
}

static void test_compress(void) {
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    cwist_middleware_func mw = cwist_mw_compress(0);

    cwist_http_request *req = make_request();
    cwist_http_response *res = cwist_http_response_create();
    REQUIRE(res);
    run(mw, false, req, res);
    const char *enc = cwist_http_header_get(res->headers, "Content-Encoding");
    REQUIRE(enc && strcmp(enc, "gzip") == 0);
    REQUIRE(res->body->size < BODY_LEN);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    req = make_request();
    res = cwist_http_response_create();
    REQUIRE(res);
    run(mw, true, req, res);
    REQUIRE(cwist_http_header_get(res->headers, "Content-Encoding") == NULL);
    REQUIRE(res->body->size == BODY_LEN);
    REQUIRE(memcmp(res->body->data, "abcdefghij", 10) == 0);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    cwist_compress_unregister_all();
    printf("Passed compression leaves a deferred response alone\n");
}

/* Runs @p mw with stdout redirected and returns what it printed. */
static void capture_log(cwist_middleware_func mw, bool defer, char *out, size_t out_len) {
    cwist_http_request *req = make_request();
    cwist_http_response *res = cwist_http_response_create();
    REQUIRE(res);

    fflush(stdout);
    FILE *tmp = tmpfile();
    REQUIRE(tmp);
    int saved = dup(STDOUT_FILENO);
    REQUIRE(saved >= 0);
    REQUIRE(dup2(fileno(tmp), STDOUT_FILENO) >= 0);
    run(mw, defer, req, res);
    fflush(stdout);
    REQUIRE(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    rewind(tmp);
    size_t n = fread(out, 1, out_len - 1, tmp);
    out[n] = '\0';
    fclose(tmp);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void expect_contains(const char *log, const char *want) {
    if (!strstr(log, want)) {
        fprintf(stderr, "FAIL: log line \"%s\" lacks \"%s\"\n", log, want);
        REQUIRE(0);
    }
}

static void test_access_logs(void) {
    char log[1024];

    capture_log(cwist_mw_access_log(CWIST_LOG_COMMON), false, log, sizeof(log));
    expect_contains(log, "\" 200 4095\n");
    capture_log(cwist_mw_access_log(CWIST_LOG_COMMON), true, log, sizeof(log));
    expect_contains(log, "\" - -\n");
    expect_contains(log, " - rid-from-request [");

    capture_log(cwist_mw_access_log(CWIST_LOG_COMBINED), false, log, sizeof(log));
    expect_contains(log, "\" 200 4095 \"-\" \"-\"\n");
    capture_log(cwist_mw_access_log(CWIST_LOG_COMBINED), true, log, sizeof(log));
    expect_contains(log, "\" - - \"-\" \"-\"\n");

    capture_log(cwist_mw_access_log(CWIST_LOG_JSON), false, log, sizeof(log));
    expect_contains(log, "\"status\":200,");
    expect_contains(log, "\"res_bytes\":4095}");
    capture_log(cwist_mw_access_log(CWIST_LOG_JSON), true, log, sizeof(log));
    expect_contains(log, "\"status\":null,");
    expect_contains(log, "\"res_bytes\":null}");
    expect_contains(log, "\"rid\":\"rid-from-request\"");

    printf("Passed access logs do not read a deferred response\n");
}

int main(void) {
    printf("Testing built-in middleware with a deferred response...\n");
    test_compress();
    test_access_logs();
    printf("All deferred built-in middleware tests passed.\n");
    return 0;
}
