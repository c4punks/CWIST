/* libFuzzer entry point for HTTP/1.1 request/response parsing and framing. */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/cookie.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/multipart.h>
#include <cwist/net/http/query.h>

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_devnull_fd = -1;

static int fuzz_devnull_fd(void) {
    if (g_devnull_fd < 0) {
        g_devnull_fd = open("/dev/null", O_RDWR);
    }
    return g_devnull_fd;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 256 * 1024) return 0;

    /* 1. Direct length-bounded request parser (SIMD CRLF + header validation). */
    cwist_http_request *req = cwist_http_parse_request_len((const char *)data, size);
    if (req) {
        (void)cwist_http_header_get(req->headers, "host");
        (void)cwist_http_header_get(req->headers, "content-type");
        (void)cwist_http_header_get(req->headers, "cookie");
        const char *ct = cwist_http_header_get(req->headers, "content-type");
        if (ct) {
            char *boundary = cwist_multipart_extract_boundary(ct);
            if (boundary) {
                if (req->body && req->body->data) {
                    cwist_multipart_result *mp =
                        cwist_multipart_parse(req->body->data, req->body->size, boundary);
                    cwist_multipart_result_destroy(mp);
                }
                cwist_free(boundary);
            }
        }
        cwist_http_request_destroy(req);
    }

    /* 2. Non-blocking incremental C1M request assembly (chunked, CL, pipelining). */
    cwist_http_async_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.fd = fuzz_devnull_fd();
    conn.peer_eof = true;
    conn.cap = size + 1;
    conn.rbuf = (char *)cwist_alloc(conn.cap);
    if (conn.rbuf) {
        if (size > 0) memcpy(conn.rbuf, data, size);
        conn.rbuf[size] = '\0';
        conn.len = size;

        for (int iter = 0; iter < 16; iter++) {
            cwist_http_request *nb_req = NULL;
            cwist_http_parse_error_t perr = CWIST_HTTP_PARSE_OK;
            cwist_recv_status_t st = cwist_http_receive_request_nb(&conn, &nb_req, &perr);
            if (st != CWIST_RECV_OK || !nb_req) break;
            cwist_http_request_destroy(nb_req);
        }
        cwist_free(conn.rbuf);
    }
    cwist_free(conn.obuf);

    /* 3. NUL-terminated auxiliary parsers: query, cookie, HTTP-date, multipart. */
    char *str = (char *)cwist_alloc(size + 1);
    if (!str) return 0;
    if (size > 0) memcpy(str, data, size);
    str[size] = '\0';

    (void)cwist_http_parse_date(str);

    cwist_query_map *qmap = cwist_query_map_create();
    if (qmap) {
        cwist_query_map_parse(qmap, str);
        (void)cwist_query_map_get(qmap, "a");
        cwist_query_map_destroy(qmap);
    }

    cwist_query_map *cmap = cwist_query_map_create();
    if (cmap) {
        cwist_cookie_parse(cmap, str);
        (void)cwist_cookie_get(cmap, "session");
        cwist_query_map_destroy(cmap);
    }

    if (size <= 512) {
        char *enc = cwist_cookie_encode(str);
        if (enc) {
            char dec[1024];
            (void)cwist_cookie_decode(enc, dec, sizeof(dec));
            cwist_free(enc);
        }
    }

    cwist_multipart_result *mp_fixed =
        cwist_multipart_parse((const char *)data, size, "cwist-fuzz-boundary");
    cwist_multipart_result_destroy(mp_fixed);

    /* 4. Response header & chunked stream serialization. */
    cwist_http_response *res = cwist_http_response_create();
    if (res) {
        if (size >= 2) {
            res->status_code = (cwist_http_status_t)(100 + ((data[0] << 8 | data[1]) % 500));
            res->keep_alive = (data[0] & 1) != 0;
        }
        if ((size & 1) == 0 && cwist_http_response_stream_begin(res) == 0) {
            size_t half = size / 2;
            (void)cwist_http_response_stream_write(res, (const char *)data, half);
            (void)cwist_http_response_stream_write(res, (const char *)data + half, size - half);
            (void)cwist_http_response_stream_end(res);
        } else if (res->body) {
            cwist_sstring_assign_len(res->body, (char *)data, size);
        }
        char *serialized = NULL;
        size_t serialized_len = 0;
        if (cwist_http_response_serialize(res, &serialized, &serialized_len) == 0) {
            cwist_free(serialized);
        }
        cwist_http_response_destroy(res);
    }

    cwist_free(str);
    return 0;
}
