#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* A failed allocation while assembling a chunked request body must reject
 * the request, not deliver a body with the chunk silently missing.
 *
 * cwist_sstring_append_len() reports a failed allocation on the JSON error
 * channel, where err_i8 is 0, so a caller that only looked at err_i8 took
 * the failure for success. This test makes exactly one allocation fail: the
 * realloc() that grows the body from 16 to 65536 bytes when the second
 * chunk (40000 bytes) arrives. The binary is linked with
 * -Wl,--wrap=realloc (GNU ld and lld) so the wrapper sees every realloc()
 * made inside libcwist; on other linkers the Makefile builds it without the
 * wrapper and the test only reports that it was skipped. */
#include <cwist/net/http/http.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SECOND_CHUNK 40000
/* 16-byte initial capacity doubled until it holds 5 + 40000 bytes, plus the
 * terminating NUL: see cwist_sstring_reserve(). */
#define GROWTH_REQUEST (65536 + 1)

#ifdef CWIST_TEST_WRAP_REALLOC
static size_t g_fail_size;
static int g_failures;

void *__real_realloc(void *ptr, size_t size);

void *__wrap_realloc(void *ptr, size_t size) {
    if (g_fail_size && ptr && size == g_fail_size) {
        g_failures++;
        return NULL;
    }
    return __real_realloc(ptr, size);
}
#endif

static cwist_http_request *send_chunked(cwist_http_parse_error_t *err) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    static char request[SECOND_CHUNK + 512];
    int n = snprintf(request, sizeof(request),
                     "POST /upload HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "5\r\nHello\r\n"
                     "%x\r\n",
                     SECOND_CHUNK);
    memset(request + n, 'x', SECOND_CHUNK);
    n += SECOND_CHUNK;
    n += snprintf(request + n, sizeof(request) - (size_t)n, "\r\n0\r\n\r\n");
    assert(write(sv[1], request, (size_t)n) == n);
    close(sv[1]);

    static char buf[128 * 1024];
    size_t buf_len = 0;
    *err = CWIST_HTTP_PARSE_OK;
    cwist_http_request *req = cwist_http_receive_request(sv[0], buf, sizeof(buf), &buf_len, err);
    close(sv[0]);
    return req;
}

int main(void) {
#ifndef CWIST_TEST_WRAP_REALLOC
    printf("test_http_chunked_alloc_failure: skipped (linker has no --wrap)\n");
    return 0;
#else
    cwist_http_parse_error_t err;

    /* Normal run: both chunks arrive. */
    cwist_http_request *req = send_chunked(&err);
    assert(req != NULL && req->body != NULL);
    assert(req->body->size == 5 + SECOND_CHUNK);
    assert(memcmp(req->body->data, "Hello", 5) == 0 && req->body->data[5] == 'x');
    cwist_http_request_destroy(req);
    printf("Passed complete chunked body\n");

    /* Growing the body for the second chunk fails: the request is refused. */
    g_fail_size = GROWTH_REQUEST;
    req = send_chunked(&err);
    g_fail_size = 0;
    assert(g_failures == 1);
    if (req) {
        fprintf(stderr, "FAIL: request accepted with a %zu-byte body after the append failed\n",
                req->body ? req->body->size : (size_t)0);
        cwist_http_request_destroy(req);
        return 1;
    }
    assert(err == CWIST_HTTP_PARSE_MALFORMED);
    printf("Passed failed chunk append rejects the request\n");

    printf("All chunked allocation-failure tests passed!\n");
    return 0;
#endif
}
