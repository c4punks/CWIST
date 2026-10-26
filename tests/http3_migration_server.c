/** @file http3_migration_server.c
 * @brief HTTP/3 echo server for the connection migration test.
 */
#include <cwist/net/http/http3.h>
#include <lsquic.h>
#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static cwist_http3_context *server_ctx;
static lsquic_conn_t *connections[16];
static unsigned connection_count;

static void stop_server(int sig) {
    (void)sig;
    if (server_ctx) __atomic_store_n(&server_ctx->running, 0, __ATOMIC_RELEASE);
}

static void echo_request(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    lsquic_conn_t *conn = lsquic_stream_conn((lsquic_stream_t *)req->private_data);
    unsigned id;
    for (id = 0; id < connection_count; ++id) {
        if (connections[id] == conn) break;
    }
    assert(id < sizeof(connections) / sizeof(connections[0]));
    if (id == connection_count) connections[connection_count++] = conn;
    char value[32];
    snprintf(value, sizeof(value), "%u", id);
    cwist_http_header_add(&res->headers, "x-test-connection", value);
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign_len(res->body, req->body->data, req->body->size);
}

int main(void) {
    cwist_error_t err = cwist_http3_init_context_ephemeral(&server_ctx);
    if (err.error.err_i16 != 0 || !server_ctx) return 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return 1;
    signal(SIGTERM, stop_server);
    signal(SIGINT, stop_server);
    printf("READY %u\n", ntohs(addr.sin_port));
    fflush(stdout);
    err = cwist_http3_server_loop(fd, server_ctx, echo_request, NULL);
    cwist_http3_destroy_context(server_ctx);
    close(fd);
    return err.error.err_i16 != 0;
}
