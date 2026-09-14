#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static void require_ok(cwist_error_t err) {
    int ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    REQUIRE(ok);
}

static unsigned releases;
static void release_body(const void *ptr, size_t len, void *ctx) {
    (void)ptr;
    (void)len;
    (void)ctx;
    ++releases;
}

static void check_response(int custom, int pointer, int keep_alive, size_t body_len) {
    static const unsigned char body[] = {'a', 0, 'b', 0xff};
    cwist_http_response *res = cwist_http_response_create();
    REQUIRE(res != NULL);
    res->keep_alive = keep_alive != 0;
    releases = 0;
    if (pointer) {
        cwist_http_response_set_body_ptr_managed(res, body, body_len, release_body, NULL);
    } else {
        require_ok(cwist_sstring_assign_len(res->body, (const char *)body, body_len));
    }
    if (custom) {
        require_ok(cwist_http_header_add(&res->headers, "Date", "Sun, 13 Sep 2026 00:00:00 GMT"));
        require_ok(cwist_http_header_add(&res->headers, "X-Test", "marker"));
    }
    cwist_sstring *wire = cwist_http_stringify_response(res);
    REQUIRE(wire != NULL && wire->data != NULL);
    /* Independent wire boundary and payload oracle, not another serializer. */
    size_t header_len = 0;
    for (size_t i = 0; i + 4 <= wire->size; ++i) {
        if (memcmp(wire->data + i, "\r\n\r\n", 4) == 0) {
            header_len = i + 4;
            break;
        }
    }
    REQUIRE(header_len != 0);
    REQUIRE(wire->size == header_len + body_len);
    REQUIRE(memcmp(wire->data + header_len, body, body_len) == 0);
    REQUIRE(wire->data[wire->size] == '\0');
    REQUIRE(releases == 0);
    if (!custom) {
        char expected[128];
        int n = snprintf(expected, sizeof(expected),
            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
            body_len, keep_alive ? "keep-alive" : "close");
        REQUIRE(n > 0 && (size_t)n < sizeof(expected));
        REQUIRE(header_len == (size_t)n);
        REQUIRE(memcmp(wire->data, expected, header_len) == 0);
    } else {
        REQUIRE(strstr(wire->data, "X-Test: marker\r\n") != NULL);
        REQUIRE(strstr(wire->data, "Date: Sun, 13 Sep 2026 00:00:00 GMT\r\n") != NULL);
    }
    cwist_sstring_destroy(wire);
    REQUIRE(releases == 0);
    cwist_http_response_destroy(res);
    REQUIRE(releases == (pointer ? 1u : 0u));
}

int main(void) {
    REQUIRE(cwist_http_stringify_response(NULL) == NULL);
    check_response(0, 0, 1, 0);
    check_response(0, 0, 1, 4);
    check_response(0, 1, 1, 4);
    check_response(0, 0, 0, 4);
    check_response(1, 0, 1, 4);
    check_response(1, 1, 0, 4);
    puts("test_http_stringify: 6 wire cases and NULL input passed");
    return 0;
}
