/**
 * WebTransport echo server for scripts/ci/webtransport_e2e.py.
 *
 * Usage: webtransport_echo_server <port> <cert> <key>
 *
 * Sessions on /wt are accepted and any other path is refused with 403.
 * Bidirectional streams are echoed back and then finished. Datagrams are
 * echoed too, except a datagram reading "close", which makes the server
 * close the session with code 7 and reason "bye". Prints READY once it is
 * listening.
 */
#include <cwist/net/http/http3.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *g_session;

static void on_session(cwist_http_request *req, cwist_http_response *res, void *session) {
    if (!req->path || strcmp(req->path->data, "/wt") != 0) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        return;
    }
    res->status_code = CWIST_HTTP_OK;
    g_session = session;
}

static void on_stream(void *stream, void *user_ctx) {
    (void)user_ctx;
    char buf[4096];
    ssize_t n;
    while ((n = cwist_webtransport_read(stream, buf, sizeof(buf))) > 0) {
        if (cwist_webtransport_write(stream, buf, (size_t)n) != n) {
            fprintf(stderr, "short write\n");
        }
    }
    if (n == 0) {
        cwist_webtransport_flush(stream);
        cwist_webtransport_close_stream(stream);
    }
}

static void on_datagram(const void *data, size_t len, void *user_ctx) {
    (void)user_ctx;
    if (!g_session) return;
    if (len == 5 && memcmp(data, "close", 5) == 0) {
        cwist_webtransport_close_session(g_session, 7, "bye");
        return;
    }
    cwist_webtransport_send_datagram(g_session, data, len);
}

static void on_request(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "not webtransport\n");
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <cert> <key>\n", argv[0]);
        return 2;
    }
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, argv[2], argv[3]);
    if (err.error.err_i16 != 0 || !ctx) {
        fprintf(stderr, "init_context failed\n");
        return 1;
    }
    cwist_http3_set_webtransport_handler(ctx, on_session);
    cwist_webtransport_set_new_stream_handler(ctx, on_stream, NULL);
    cwist_http3_set_datagram_callback(ctx, on_datagram, NULL);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)atoi(argv[1]));
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    printf("READY\n");
    fflush(stdout);
    cwist_http3_server_loop(fd, ctx, on_request, NULL);
    cwist_http3_destroy_context(ctx);
    close(fd);
    return 0;
}
