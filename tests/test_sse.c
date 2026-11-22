#include <cwist/net/http/sse.h>
#include <cwist/core/sstring/sstring.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    cwist_sse_stream_t *stream;
    atomic_int done;
} sse_sender;

static void *blocked_sender(void *arg) {
    sse_sender *sender = arg;
    char *payload = malloc(1024 * 1024 + 1);
    assert(payload != NULL);
    memset(payload, 'x', 1024 * 1024);
    payload[1024 * 1024] = '\0';
    while (cwist_sse_stream_send(sender->stream, "message", NULL, -1, payload).error.err_i16 == 0) {
    }
    free(payload);
    atomic_store(&sender->done, 1);
    return NULL;
}

int main(void) {
    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    assert(cwist_sse_response_init(res).error.err_i16 == 0);
    assert(cwist_sse_response_event(res, "update", "42", 1500, "first\nsecond").error.err_i16 == 0);
    assert(cwist_sse_response_comment(res, "heartbeat").error.err_i16 == 0);
    assert(cwist_sse_response_event(res, "safe", NULL, -1, " leading\rhidden: no\r\nlast").error.err_i16 == 0);
    cwist_sse_event_t typed = CWIST_SSE_NAMED("typed", "payload");
    assert(cwist_sse_response_write(res, &typed).error.err_i16 == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "Content-Type"),
                  "text/event-stream; charset=utf-8") == 0);
    assert(
        strcmp(
            res->body->data,
            "id: 42\nevent: update\nretry: 1500\ndata: first\ndata: second\n\n: heartbeat\n\n"
            "event: safe\ndata:  leading\ndata: hidden: no\ndata: last\n\n"
            "event: typed\ndata: payload\n\n") ==
        0);
    cwist_http_response_destroy(res);

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    cwist_http_request *req = cwist_http_request_create();
    req->client_fd = pair[0];
    cwist_sse_stream_t *stream = cwist_sse_stream_open(req);
    assert(stream != NULL && req->upgraded);
    assert(cwist_sse_stream_send(stream, "message", "7", -1, "ok").error.err_i16 == 0);
    char received[512] = {0};
    ssize_t n = read(pair[1], received, sizeof(received) - 1);
    assert(n > 0);
    assert(strstr(received, "Content-Type: text/event-stream") != NULL);
    assert(strstr(received, "id: 7\nevent: message\ndata: ok\n\n") != NULL);
    cwist_sse_event_t live_event = CWIST_SSE_EVENT("typed payload");
    assert(cwist_sse_stream_write(stream, &live_event).error.err_i16 == 0);
    assert(cwist_sse_stream_comment(stream, "keepalive").error.err_i16 == 0);
    cwist_sse_stream_close(stream);
    cwist_http_request_destroy(req);
    close(pair[0]);
    close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    req = cwist_http_request_create();
    req->client_fd = pair[0];
    stream = cwist_sse_stream_open(req);
    assert(stream != NULL);
    sse_sender sender = {.stream = stream};
    atomic_init(&sender.done, 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocked_sender, &sender) == 0);
    struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
    cwist_sse_stream_close(stream);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&sender.done));
    cwist_http_request_destroy(req);
    close(pair[0]);
    close(pair[1]);
    /* #956: live streams refuse TLS, HTTP/2 and HTTP/3 requests and write
     * nothing to their socket. */
    for (int kind = 0; kind < 3; kind++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        req = cwist_http_request_create();
        req->client_fd = pair[0];
        int marker = 0;
        if (kind == 0) req->https_conn = &marker;
        if (kind == 1) {
            req->stream_id = 1;
            cwist_sstring_assign(req->version, "HTTP/2");
        }
        if (kind == 2) cwist_sstring_assign(req->version, "HTTP/3");
        assert(cwist_sse_stream_open(req) == NULL);
        assert(!req->upgraded);
        char byte;
        assert(recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0);
        req->https_conn = NULL;
        cwist_http_request_destroy(req);
        close(pair[0]);
        close(pair[1]);
    }
    puts("All SSE tests passed.");
    return 0;
}
