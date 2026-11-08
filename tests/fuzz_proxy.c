/* libFuzzer entry point for reverse-proxy URL resolution, path/query/header
 * sanitizing, request job building, WebSocket/CONNECT passthrough validation,
 * and upstream response header/body callbacks. */
#include <curl/curl.h>

#define curl_easy_perform(c) CURLE_COULDNT_CONNECT
#include "../src/net/proxy/proxy.c"
#undef curl_easy_perform

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool g_fuzz_proxy_inited = false;
static cwist_app g_fuzz_front_app;

static void fuzz_proxy_cleanup(void) {
    cwist_proxy_reset();
}

static void fuzz_proxy_ensure_init(void) {
    if (g_fuzz_proxy_inited) return;
    g_fuzz_proxy_inited = true;

    cwist_error_t err;
    err = cwist_proxy_route("http://front.test:8080", "/api", "http://127.0.0.1:3000", "/v1");
    cwist_error_dispose(&err);
    err = cwist_proxy_route("http://front.test:8080", "/", "http://127.0.0.1:3001", "/");
    cwist_error_dispose(&err);
    err = cwist_proxy_route("https://secure.test:8443", "/app", "https://127.0.0.1:3443", "/");
    cwist_error_dispose(&err);
    err = cwist_proxy_port_forward(8081, 4000);
    cwist_error_dispose(&err);

    memset(&g_fuzz_front_app, 0, sizeof(g_fuzz_front_app));
    g_fuzz_front_app.port = 8080;
    atexit(fuzz_proxy_cleanup);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 128 * 1024) return 0;
    fuzz_proxy_ensure_init();

    char *str = (char *)cwist_alloc(size + 1);
    if (!str) return 0;
    if (size > 0) memcpy(str, data, size);
    str[size] = '\0';

    /* 1. Path, query, header, authority, and route validators. */
    (void)cwist_proxy_path_is_safe(str);
    (void)cwist_proxy_query_is_safe(str);
    (void)cwist_proxy_header_name_ok(str);
    (void)cwist_proxy_header_value_ok(str);

    char *norm_route = proxy_normalize_route(str);
    cwist_free(norm_route);

    char *parsed_target = proxy_parse_target(str);
    cwist_free(parsed_target);

    {
        char hk[256];
        unsigned short hport = 0;
        (void)proxy_host_key(str, false, hk, sizeof(hk), &hport);
        (void)proxy_host_key(str, true, hk, sizeof(hk), &hport);
    }

    /* 2. URL resolution across configured rules. */
    {
        int status = 0;
        const char *host = (size > 0 && (data[0] & 1)) ? "front.test:8080" : str;
        unsigned short port = (size > 0 && (data[0] & 2)) ? 8081 : 8080;
        char *url = cwist_proxy_resolve_url(port, host, str, NULL, &status);
        cwist_free(url);
        url = cwist_proxy_resolve_url(port, "front.test:8080", "/api/items", str, &status);
        cwist_free(url);
    }

    /* 3. Body growth calculation under arbitrary sizes. */
    if (size >= 12) {
        size_t cap = ((size_t)data[0] << 16) | ((size_t)data[1] << 8) | data[2];
        size_t need =
            ((size_t)data[3] << 24) | ((size_t)data[4] << 16) | ((size_t)data[5] << 8) | data[6];
        size_t max =
            ((size_t)data[7] << 24) | ((size_t)data[8] << 16) | ((size_t)data[9] << 8) | data[10];
        (void)cwist_proxy_body_grow(cap, need, max);
    }

    /* 4. Front request -> proxy_build_job / WS & CONNECT passthrough validators. */
    cwist_http_request *req = cwist_http_parse_request_len((const char *)data, size);
    if (req) {
        req->app = &g_fuzz_front_app;
        req->client_fd = -1;

        proxy_job *job = (proxy_job *)cwist_alloc(sizeof(*job));
        if (job) {
            memset(job, 0, sizeof(*job));
            (void)proxy_build_job(req, job);
            proxy_job_free(job);
        }

        cwist_http_response *res = cwist_http_response_create();
        if (res) {
            if (req->method == CWIST_HTTP_CONNECT) {
                proxy_connect_passthrough(req, res);
            } else {
                proxy_ws_passthrough(req, res);
            }
            cwist_http_response_destroy(res);
        }
        cwist_http_request_destroy(req);
    }

    /* 5. Upstream header and body callbacks. */
    {
        proxy_job dummy_job;
        memset(&dummy_job, 0, sizeof(dummy_job));
        dummy_job.front_port = 8080;
        dummy_job.head = (size > 0 && (data[0] & 1));

        cwist_http_response *res = cwist_http_response_create();
        if (res) {
            proxy_upstream_res u;
            memset(&u, 0, sizeof(u));
            u.max = 256 * 1024;
            u.can_stream = false;
            u.job = &dummy_job;
            u.res = res;

            size_t pos = 0;
            while (pos < size) {
                size_t line_len = (size_t)data[pos] + 1;
                pos++;
                if (line_len > size - pos) line_len = size - pos;
                if (proxy_upstream_header_cb((char *)(data + pos), 1, line_len, &u) == 0) break;
                pos += line_len;
            }
            if (!u.bad_header && !u.too_big && size > 0) {
                (void)proxy_upstream_write_cb((void *)data, 1, size / 2, &u);
                (void)proxy_prepare_response(&u, &dummy_job, res, false);
            }
            cwist_http_header_free_all(u.headers);
            cwist_free(u.body);
            cwist_http_response_destroy(res);
        }
    }

    cwist_free(str);
    return 0;
}
