/**
 * @file test_rate_limit.c
 * @brief Unit tests for token-bucket rate limiting middleware.
 */

#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

static int test_peer_fd(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &addr_len) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    assert(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    close(client);
    close(listener);
    return peer;
}

static void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
}

void test_rate_limit_blocks_after_burst(void) {
    printf("test_rate_limit_blocks_after_burst...\n");
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = test_peer_fd();
    assert(req->client_fd >= 0);

    cwist_middleware_func mw = cwist_mw_rate_limit_ip(2); /* 2 req/min */
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next); /* 3rd should be blocked */
    assert(res->status_code == 429);

    close(req->client_fd);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_mw_rate_limit_reset();
    printf("  OK\n");
}

void test_rate_limit_respects_param(void) {
    printf("test_rate_limit_respects_param...\n");
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = test_peer_fd();
    assert(req->client_fd >= 0);

    cwist_middleware_func mw = cwist_mw_rate_limit_ip(1); /* 1 req/min */
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next); /* 2nd blocked immediately */
    assert(res->status_code == 429);

    close(req->client_fd);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("  OK\n");
}

int main(void) {
    test_rate_limit_blocks_after_burst();
    test_rate_limit_respects_param();
    printf("All rate limit tests passed.\n");
    return 0;
}
