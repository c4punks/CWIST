/**
 * @file main.c
 * @brief WebRTC DataChannel echo server: SDP signaling over HTTP POST,
 * ICE-lite + DTLS + SCTP DataChannel echo over UDP.
 *
 * Open http://localhost:8080/ in a browser, click Connect, and type
 * messages: the server echoes every DataChannel message back.
 */
#include <cwist/app.h>
#include <cwist/net/webrtc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cwist_webrtc_ctx *g_webrtc;

static void on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                       size_t len, void *user) {
    (void)user;
    (void)channel;
    printf("[webrtc] message (%zu bytes): %.*s\n", len, (int)len, (const char *)data);
    cwist_webrtc_conn_send(conn, channel, data, len, CWIST_WEBRTC_DATA_BINARY);
}

static void on_channel(cwist_webrtc_conn *conn, uint16_t channel, const char *label,
                       void *user) {
    (void)conn;
    (void)user;
    printf("[webrtc] channel %u opened by peer (label=%s)\n", channel, label);
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    FILE *f = fopen("index.html", "rb");
    if (!f) {
        /* Fall back to the source directory layout. */
        f = fopen("example/webrtc/index.html", "rb");
    }
    if (!f) {
        cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
        cwist_sstring_assign(res->body, "index.html not found");
        return;
    }
    char html[16384];
    size_t n = fread(html, 1, sizeof(html) - 1, f);
    fclose(f);
    html[n] = '\0';
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body, html);
}

static void offer_handler(cwist_http_request *req, cwist_http_response *res) {
    static char answer[4096];
    if (cwist_webrtc_handle_offer(g_webrtc, req->body->data, answer, sizeof(answer)) < 0) {
        cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
        cwist_sstring_assign(res->body, "invalid SDP offer");
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        return;
    }
    printf("[webrtc] answered offer; UDP port %u, fingerprint %s\n",
           cwist_webrtc_ctx_port(g_webrtc), cwist_webrtc_ctx_fingerprint(g_webrtc));
    cwist_http_header_add(&res->headers, "Content-Type", "application/sdp");
    cwist_sstring_assign(res->body, answer);
}

int main(void) {
    g_webrtc = cwist_webrtc_ctx_new(0);
    if (!g_webrtc) {
        fprintf(stderr, "failed to create webrtc ctx\n");
        return 1;
    }
    cwist_webrtc_ctx_set_message_handler(g_webrtc, &on_message, NULL);
    cwist_webrtc_ctx_set_channel_handler(g_webrtc, &on_channel, NULL);

    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", index_handler);
    cwist_app_post(app, "/offer", offer_handler);
    printf("signaling: http://localhost:8080/ (webrtc UDP port %u)\n",
           cwist_webrtc_ctx_port(g_webrtc));
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    cwist_webrtc_ctx_free(g_webrtc);
    return 0;
}
