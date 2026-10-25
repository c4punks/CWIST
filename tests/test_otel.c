/**
 * @file test_otel.c
 * @brief OpenTelemetry middleware: W3C traceparent handling, the trace id
 *        handlers read, and the OTLP/HTTP export a collector receives.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/sys/app/test_client.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define FAIL_IF(cond, msg)                      \
    do {                                        \
        if (cond) {                             \
            fprintf(stderr, "FAIL: %s\n", msg); \
            return 1;                           \
        }                                       \
    } while (0)

static void trace_id_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_mw_otel_get_trace_id(req);
    cwist_sstring_assign(res->body, id ? id : "none");
    res->status_code = CWIST_HTTP_OK;
}

static int is_lower_hex(const char *s, size_t len) {
    if (strlen(s) != len) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    }
    return 1;
}

/* GET / with an optional traceparent header; copies the response body. */
static int get_trace_id(cwist_test_client *client, const char *traceparent, char *out,
                        size_t cap) {
    cwist_test_client_kv hdr = {"traceparent", traceparent};
    cwist_test_client_request_options opts = {
        .headers = traceparent ? &hdr : NULL,
        .header_count = traceparent ? 1 : 0,
    };
    cwist_http_response *res = cwist_test_client_request_ex(client, CWIST_HTTP_GET, "/", &opts);
    if (!res) return -1;
    int ok = res->status_code == CWIST_HTTP_OK && res->body && res->body->data;
    if (ok) snprintf(out, cap, "%s", res->body->data);
    cwist_http_response_destroy(res);
    return ok ? 0 : -1;
}

/* One-shot collector: a listening socket on an ephemeral loopback port. */
static int collector_listen(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, len) != 0 || listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Accept one export, answer 200 and return what was received (headers+body). */
static int collector_receive(int lfd, char *buf, size_t cap) {
    struct pollfd p = {.fd = lfd, .events = POLLIN};
    if (poll(&p, 1, 5000) != 1) return -1;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return -1;
    size_t got = 0;
    p.fd = fd;
    while (got + 1 < cap && poll(&p, 1, 3000) == 1) {
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "]}]}]}")) break; /* end of the OTLP JSON body */
    }
    buf[got] = '\0';
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    (void)!write(fd, ok, sizeof(ok) - 1);
    close(fd);
    return 0;
}

int main(void) {
    int port = 0;
    int lfd = collector_listen(&port);
    FAIL_IF(lfd < 0, "collector socket");
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/v1/traces", port);
    cwist_otel_cfg_t cfg = {.endpoint = endpoint, .service_name = "otel-test", .batch_size = 1};

    cwist_app *app = cwist_app_create();
    FAIL_IF(!app, "app create");
    cwist_middleware_func mw = cwist_mw_otel_trace(&cfg);
    FAIL_IF(!mw, "middleware create");
    cwist_app_use(app, mw);
    cwist_app_get(app, "/", trace_id_handler);

    cwist_test_client *client = cwist_test_client_create(app);
    FAIL_IF(!client, "client create");

    char id[64];
    FAIL_IF(get_trace_id(client, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", id,
                         sizeof(id)) != 0,
            "request with traceparent");
    FAIL_IF(strcmp(id, "4bf92f3577b34da6a3ce929d0e0e4736") != 0, "trace id from traceparent");

    static char export[16384];
    FAIL_IF(collector_receive(lfd, export, sizeof(export)) != 0, "span exported to collector");
    FAIL_IF(!strstr(export, "POST /v1/traces"), "OTLP/HTTP POST");
    FAIL_IF(!strstr(export, "\"otel-test\""), "service.name");
    FAIL_IF(!strstr(export, "\"traceId\":\"4bf92f3577b34da6a3ce929d0e0e4736\""), "span trace id");
    FAIL_IF(!strstr(export, "\"parentSpanId\":\"00f067aa0ba902b7\""), "parent span id");
    FAIL_IF(!strstr(export, "\"startTimeUnixNano\""), "span timestamps");
    close(lfd);

    FAIL_IF(get_trace_id(client, "00-00000000000000000000000000000000-00f067aa0ba902b7-01", id,
                         sizeof(id)) != 0 ||
                strcmp(id, "00000000000000000000000000000000") == 0 || !is_lower_hex(id, 32),
            "all-zero trace id is rejected and replaced");
    FAIL_IF(get_trace_id(client, "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01", id,
                         sizeof(id)) != 0 ||
                strcmp(id, "4bf92f3577b34da6a3ce929d0e0e4736") == 0,
            "uppercase traceparent is rejected");

    FAIL_IF(get_trace_id(client, NULL, id, sizeof(id)) != 0, "request without traceparent");
    FAIL_IF(!is_lower_hex(id, 32), "fresh 128-bit trace id without traceparent");

    FAIL_IF(cwist_mw_otel_get_trace_id(NULL), "NULL request");

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    printf("test_otel: OK\n");
    return 0;
}
