/**
 * @file test_compress.c
 * @brief Test compression middleware with zlib and Brotli backends.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include <brotli/decode.h>

static int fail_assign_len;
cwist_error_t __real_cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);
cwist_error_t __wrap_cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len) {
    if (fail_assign_len) return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = -1};
    return __real_cwist_sstring_assign_len(str, data, len);
}

static void large_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Build a large repetitive body that compresses well */
    char buf[4096];
    for (size_t i = 0; i < sizeof(buf) - 1; ++i) {
        buf[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    }
    buf[sizeof(buf) - 1] = '\0';
    cwist_sstring_assign(res->body, buf);
    res->status_code = CWIST_HTTP_OK;
}

static int response_mode;

static void eligibility_handler(cwist_http_request *req, cwist_http_response *res) {
    large_handler(req, res);
    if (response_mode == 1) {
        res->status_code = CWIST_HTTP_PARTIAL_CONTENT;
        cwist_http_header_add(&res->headers, "Content-Range", "bytes 0-4094/4095");
    } else if (response_mode == 2) {
        cwist_http_header_add(&res->headers, "Content-Encoding", "gzip");
    }
    cwist_http_header_add(&res->headers, "ETag", "\"strong\"");
}

static void verify_original(const unsigned char *data, size_t len) {
    assert(len == 4095);
    for (size_t i = 0; i < len; ++i) {
        assert(data[i] == "abcdefghijklmnopqrstuvwxyz"[i % 26]);
    }
}

static void test_gzip(void) {
    printf("Testing gzip compression...\n");

    cwist_compress_register_backend(cwist_compress_backend_gzip());

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/", large_handler);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "gzip");

    cwist_app_dispatch(app, req, res);

    assert(res->status_code == CWIST_HTTP_OK);
    char *ce = cwist_http_header_get(res->headers, "Content-Encoding");
    assert(ce != NULL);
    assert(strcmp(ce, "gzip") == 0);
    assert(res->body->size < 4096);

    z_stream zs = {0};
    assert(inflateInit2(&zs, 15 + 16) == Z_OK);
    unsigned char decompressed[8192];
    zs.next_in = (Bytef *)res->body->data;
    zs.avail_in = (uInt)res->body->size;
    zs.next_out = decompressed;
    zs.avail_out = sizeof(decompressed);
    int rc = inflate(&zs, Z_FINISH);
    assert(rc == Z_STREAM_END || rc == Z_OK);
    size_t decompressed_len = sizeof(decompressed) - zs.avail_out;
    inflateEnd(&zs);
    verify_original(decompressed, decompressed_len);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    req = cwist_http_request_create();
    res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "gzip");
    fail_assign_len = 1;
    cwist_app_dispatch(app, req, res);
    fail_assign_len = 0;
    const char *failed_encoding = cwist_http_header_get(res->headers, "Content-Encoding");
    assert(failed_encoding == NULL || strcmp(failed_encoding, "gzip") != 0);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_app_destroy(app);
    cwist_compress_unregister_all();

    printf("Passed gzip compression test.\n");
}

static void test_brotli(void) {
    printf("Testing Brotli compression...\n");

    /* Register Brotli first so it is preferred when both are acceptable. */
    cwist_compress_register_backend(cwist_compress_backend_brotli());
    cwist_compress_register_backend(cwist_compress_backend_gzip());

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/", large_handler);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "br, gzip");

    cwist_app_dispatch(app, req, res);

    assert(res->status_code == CWIST_HTTP_OK);
    char *ce = cwist_http_header_get(res->headers, "Content-Encoding");
    assert(ce != NULL);
    assert(strcmp(ce, "br") == 0);
    assert(res->body->size < 4096);

    unsigned char decompressed[8192];
    size_t decompressed_len = sizeof(decompressed);
    BrotliDecoderResult result = BrotliDecoderDecompress(
        res->body->size, (const uint8_t *)res->body->data, &decompressed_len, decompressed);
    assert(result == BROTLI_DECODER_RESULT_SUCCESS);
    verify_original(decompressed, decompressed_len);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_app_destroy(app);
    cwist_compress_unregister_all();

    printf("Passed Brotli compression test.\n");
}

static void check_encoding(const char *accept, const char *expected) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/", large_handler);
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", accept);
    cwist_app_dispatch(app, req, res);
    const char *encoding = cwist_http_header_get(res->headers, "Content-Encoding");
    if (expected)
        assert(encoding && strcmp(encoding, expected) == 0);
    else
        assert(encoding == NULL);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_app_destroy(app);
}

static void test_accept_encoding_negotiation(void) {
    printf("Testing Accept-Encoding negotiation...\n");
    cwist_compress_register_backend(cwist_compress_backend_brotli());
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    check_encoding("gzip;q=0", NULL);
    check_encoding("xgzip", NULL);
    check_encoding("*;q=0.5", "br");
    check_encoding("br;q=0, *;q=1", "gzip");
    check_encoding("br;q=0.1, gzip;q=0.9", "gzip");
    check_encoding("gzip;q=0.9, br;q=0.1", "gzip");
    check_encoding("gzip;q=0, br", "br");
    cwist_compress_unregister_all();
    printf("Passed Accept-Encoding negotiation tests.\n");
}

static cwist_http_response *dispatch_with_threshold(size_t threshold, const char *accept) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    void *ctx = NULL;
    cwist_middleware_ctx_destroy_func destroy = NULL;
    cwist_middleware_func_ex mw = cwist_mw_compress_ex(threshold, &ctx, &destroy);
    assert(mw != NULL);
    cwist_error_t err = cwist_app_use_ex(app, NULL, mw, ctx, destroy);
    assert(cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    cwist_app_get(app, "/", large_handler);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", accept);
    cwist_http_header_add(&res->headers, "Vary", "Origin");
    cwist_app_dispatch(app, req, res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    return res;
}

static bool header_has_value(const cwist_http_header_node *headers, const char *key,
                             const char *value) {
    for (const cwist_http_header_node *node = headers; node; node = node->next) {
        if (node->key && node->value && strcasecmp(node->key->data, key) == 0 &&
            strstr(node->value->data, value))
            return true;
    }
    return false;
}

static void test_vary_and_independent_thresholds(void) {
    printf("Testing Vary and independent compression thresholds...\n");
    assert(cwist_mw_compress_ex(0, NULL, NULL) == NULL);
    cwist_compress_register_backend(cwist_compress_backend_gzip());

    cwist_http_response *compressed = dispatch_with_threshold(64, "gzip");
    const char *encoding = cwist_http_header_get(compressed->headers, "Content-Encoding");
    assert(encoding && strcmp(encoding, "gzip") == 0);
    assert(header_has_value(compressed->headers, "Vary", "Origin"));
    assert(header_has_value(compressed->headers, "Vary", "Accept-Encoding"));
    cwist_http_response_destroy(compressed);

    cwist_http_response *identity = dispatch_with_threshold(4096, "gzip");
    assert(cwist_http_header_get(identity->headers, "Content-Encoding") == NULL);
    cwist_http_response_destroy(identity);

    cwist_http_response *identity_variant = dispatch_with_threshold(64, "gzip;q=0");
    assert(cwist_http_header_get(identity_variant->headers, "Content-Encoding") == NULL);
    assert(header_has_value(identity_variant->headers, "Vary", "Origin"));
    assert(header_has_value(identity_variant->headers, "Vary", "Accept-Encoding"));
    cwist_http_response_destroy(identity_variant);

    cwist_compress_unregister_all();
    printf("Passed Vary and independent threshold tests.\n");
}

static cwist_http_response *dispatch_eligibility(int mode, bool range) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/", eligibility_handler);
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "gzip");
    if (range) cwist_http_header_add(&req->headers, "Range", "bytes=0-4");
    response_mode = mode;
    cwist_app_dispatch(app, req, res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    return res;
}

static void test_compression_eligibility(void) {
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    cwist_http_response *res = dispatch_eligibility(1, false);
    assert(res->status_code == CWIST_HTTP_PARTIAL_CONTENT);
    assert(cwist_http_header_get(res->headers, "Content-Encoding") == NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "ETag"), "\"strong\"") == 0);
    assert(res->body->size == 4095);
    cwist_http_response_destroy(res);

    res = dispatch_eligibility(2, false);
    size_t encoding_count = 0;
    for (cwist_http_header_node *h = res->headers; h; h = h->next) {
        if (strcasecmp(h->key->data, "Content-Encoding") == 0) encoding_count++;
    }
    assert(encoding_count == 1);
    assert(res->body->size == 4095);
    cwist_http_response_destroy(res);

    res = dispatch_eligibility(0, false);
    assert(strcmp(cwist_http_header_get(res->headers, "Content-Encoding"), "gzip") == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "ETag"), "W/\"strong\"") == 0);
    assert(res->body->size < 4095);
    cwist_http_response_destroy(res);

    res = dispatch_eligibility(0, true);
    assert(cwist_http_header_get(res->headers, "Content-Encoding") == NULL);
    assert(res->body->size == 4095);
    cwist_http_response_destroy(res);
    cwist_compress_unregister_all();
}

int main(void) {
    test_gzip();
    test_brotli();
    test_accept_encoding_negotiation();
    test_vary_and_independent_thresholds();
    test_compression_eligibility();
    printf("Passed compression middleware tests.\n");
    return 0;
}
