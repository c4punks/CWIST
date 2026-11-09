/* libFuzzer entry point for HTTP/2 HPACK decoding and frame state machine. */
#include "../src/net/http/http2.c"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int g_devnull_fd = -1;

static int fuzz_devnull_fd(void) {
    if (g_devnull_fd < 0) {
        g_devnull_fd = open("/dev/null", O_RDWR);
    }
    return g_devnull_fd;
}

static void fuzz_h2_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    if (!res) return;
    res->status_code = CWIST_HTTP_OK;
    if (req && req->body && req->body->data && req->body->size <= 64) {
        cwist_sstring_assign_len(res->body, req->body->data, req->body->size);
    } else if (res->body) {
        cwist_sstring_assign(res->body, "ok");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 128 * 1024) return 0;

    /* 1. HPACK primitives: prefix integer, Huffman, and string literal decoding. */
    for (int prefix = 1; prefix <= 8; prefix++) {
        size_t pos = 0;
        uint32_t out_val = 0;
        (void)h2_decode_integer(data, size, &pos, prefix, &out_val);
    }

    {
        size_t huff_len = 0;
        char *decoded = h2_huffman_decode(data, size, &huff_len);
        cwist_free(decoded);
    }

    {
        size_t pos = 0;
        while (pos < size) {
            char *s = h2_decode_string(data, size, &pos);
            if (!s) break;
            cwist_free(s);
        }
    }

    /* 2. HPACK header block decoder (static + dynamic table + RFC 9113 rules). */
    for (int mode = 0; mode < 2; mode++) {
        h2_conn hc;
        h2_conn_init(&hc, NULL);
        h2_stream *s = h2_stream_create(&hc, 1);
        if (s) {
            s->req = cwist_http_request_create();
            if (s->req) {
                (void)h2_decode_header_block(&hc, s, data, size, mode == 0);
            }
        }
        (void)h2_handle_settings(&hc, data, size, false);
        if (size >= 4) {
            uint32_t sid = ((uint32_t)data[0] & 1u);
            (void)h2_handle_window_update(&hc, sid, data, 4);
        }
        h2_conn_destroy(&hc);
    }

    /* 3. Full HTTP/2 connection frame loop over prebuffered cleartext input. */
    int fd = fuzz_devnull_fd();
    if (fd >= 0) {
        size_t pre_len = CWIST_HTTP2_CONNECTION_PREFACE_LEN + size;
        char *pre_buf = (char *)cwist_alloc(pre_len);
        if (pre_buf) {
            memcpy(pre_buf, CWIST_HTTP2_CONNECTION_PREFACE, CWIST_HTTP2_CONNECTION_PREFACE_LEN);
            if (size > 0) {
                memcpy(pre_buf + CWIST_HTTP2_CONNECTION_PREFACE_LEN, data, size);
            }
            cwist_https_connection conn;
            memset(&conn, 0, sizeof(conn));
            conn.fd = fd;
            conn.ssl = NULL;
            conn.read_buf = pre_buf;
            conn.buf_len = pre_len;

            cwist_error_t err = cwist_http2_serve_connection(&conn, NULL, fuzz_h2_handler);
            cwist_error_dispose(&err);
            cwist_free(conn.read_buf);
        }
    }

    return 0;
}
