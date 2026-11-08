/* libFuzzer entry point for WebSocket handshake, UTF-8/close-code validation,
 * and incremental / blocking RFC 6455 frame and fragmentation parsing. */
#include "../src/net/websocket/websocket_async.c"

#include <cwist/net/http/http.h>
#include <cwist/net/websocket/websocket.h>
#include <sys/socket.h>

static int g_ws_sv[2] = {-1, -1};

static void fuzz_ws_cleanup(void) {
    if (g_ws_sv[0] >= 0) close(g_ws_sv[0]);
    if (g_ws_sv[1] >= 0) close(g_ws_sv[1]);
    g_ws_sv[0] = g_ws_sv[1] = -1;
}

static bool fuzz_ws_ensure_sockets(void) {
    if (g_ws_sv[0] >= 0) return true;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, g_ws_sv) != 0) return false;
    atexit(fuzz_ws_cleanup);
    return true;
}

static void fuzz_ws_drain_fd(int fd) {
    uint8_t buf[4096];
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }
}

static void fuzz_ws_on_message(cwist_websocket_async *ws, cwist_ws_frame *frame, void *user_data) {
    (void)ws;
    (void)user_data;
    cwist_websocket_frame_destroy(frame);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 128 * 1024) return 0;

    /* 1. Direct UTF-8 and CLOSE payload validators. */
    (void)cwist_ws_internal_utf8_valid(data, size);
    (void)cwist_ws_internal_close_error(data, size);

    /* 2. Handshake validator on raw HTTP request bytes and synthetic headers. */
    cwist_http_request *parsed = cwist_http_parse_request_len((const char *)data, size);
    if (parsed) {
        cwist_http_response *res = cwist_http_response_create();
        if (res) {
            (void)cwist_websocket_upgrade_response(parsed, res);
            cwist_http_response_destroy(res);
        }
        cwist_http_request_destroy(parsed);
    }

    if (size >= 4 && size <= 512) {
        cwist_http_request *req = cwist_http_request_create();
        cwist_http_response *res = cwist_http_response_create();
        if (req && res) {
            req->method = (data[0] & 1) ? CWIST_HTTP_GET : CWIST_HTTP_POST;
            cwist_sstring_assign(req->version, (data[0] & 2) ? "HTTP/1.1" : "HTTP/1.0");
            char *key = (char *)cwist_alloc(size + 1);
            if (key) {
                memcpy(key, data, size);
                key[size] = '\0';
                cwist_http_header_add(&req->headers, "Host", "localhost");
                cwist_http_header_add(&req->headers, "Upgrade", (data[0] & 4) ? "websocket" : key);
                cwist_http_header_add(&req->headers, "Connection",
                                      (data[0] & 8) ? "Upgrade, keep-alive" : key);
                cwist_http_header_add(&req->headers, "Sec-WebSocket-Version",
                                      (data[0] & 16) ? "13" : key);
                cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", key);
                (void)cwist_websocket_upgrade_response(req, res);
                cwist_free(key);
            }
        }
        cwist_http_response_destroy(res);
        cwist_http_request_destroy(req);
    }

    if (!fuzz_ws_ensure_sockets()) return 0;

    /* 3. Incremental C1M frame & fragmentation parser (ws_async_feed). */
    {
        cwist_websocket_async ws;
        memset(&ws, 0, sizeof(ws));
        ws.fd = g_ws_sv[0];
        ws.on_message = fuzz_ws_on_message;

        size_t pos = 0;
        while (pos < size) {
            size_t step = (size_t)data[pos] + 1;
            pos++;
            if (step > size - pos) step = size - pos;
            if (!ws_async_stash_append(&ws, data + pos, step)) break;
            if (ws_async_feed(&ws) != 0) break;
            pos += step;
            fuzz_ws_drain_fd(g_ws_sv[1]);
            if (ws.terminate_pending) break;
        }
        ws_async_park_clear(&ws);
        cwist_free(ws.stash);
        cwist_free(ws.frag_buf);
        fuzz_ws_drain_fd(g_ws_sv[0]);
        fuzz_ws_drain_fd(g_ws_sv[1]);
    }

    /* 4. Synchronous frame parser (cwist_websocket_receive) over non-blocking socketpair. */
    if (size > 0 && size <= 32768) {
        ssize_t written = send(g_ws_sv[1], data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (written > 0) {
            cwist_websocket sync_ws;
            memset(&sync_ws, 0, sizeof(sync_ws));
            sync_ws.fd = g_ws_sv[0];
            sync_ws.frag_opcode = CWIST_WS_FRAME_CONTINUATION;

            for (int iter = 0; iter < 16; iter++) {
                cwist_ws_frame *f = cwist_websocket_receive(&sync_ws);
                if (!f) break;
                cwist_websocket_frame_destroy(f);
                fuzz_ws_drain_fd(g_ws_sv[1]);
            }
            cwist_free(sync_ws.frag_buf);
        }
        fuzz_ws_drain_fd(g_ws_sv[0]);
        fuzz_ws_drain_fd(g_ws_sv[1]);
    }

    return 0;
}
