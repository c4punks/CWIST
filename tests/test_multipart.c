/**
 * @file test_multipart.c
 * @brief Unit tests for the multipart/form-data parser wrapper.
 */

#include <cwist/net/http/multipart.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B "XbOuNdArYx"

static const cwist_multipart_field *find_field(const cwist_multipart_result *r, const char *name) {
    for (const cwist_multipart_field *f = r->fields; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

static size_t count_fields(const cwist_multipart_result *r) {
    size_t n = 0;
    for (const cwist_multipart_field *f = r->fields; f; f = f->next) n++;
    return n;
}

static void test_basic_two_fields(void) {
    printf("Testing basic two-field body...\n");
    static const char body[] =
        "--" B "\r\n"
        "Content-Disposition: form-data; name=\"greeting\"\r\n"
        "\r\n"
        "hello\r\n"
        "--" B "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file body\r\n"
        "--" B "--\r\n";

    cwist_multipart_result *r = cwist_multipart_parse(body, sizeof(body) - 1, B);
    assert(r != NULL);
    assert(count_fields(r) == 2);

    const cwist_multipart_field *g = find_field(r, "greeting");
    assert(g != NULL);
    assert(g->filename == NULL);
    assert(g->data_len == 5 && memcmp(g->data, "hello", 5) == 0);

    const cwist_multipart_field *f = find_field(r, "file");
    assert(f != NULL);
    assert(f->filename && strcmp(f->filename, "a.txt") == 0);
    assert(f->content_type && strcmp(f->content_type, "text/plain") == 0);
    assert(f->data_len == 9 && memcmp(f->data, "file body", 9) == 0);

    cwist_multipart_result_destroy(r);
    printf("  Passed.\n");
}

/* An empty header value must still terminate its header.  Before the fix the
 * next header's name was appended to the empty-valued one, so
 * "X-Empty:" followed by "Content-Disposition: ..." produced a field with no
 * name at all. */
static void test_empty_header_value_does_not_swallow_next_header(void) {
    printf("Testing empty header value followed by Content-Disposition...\n");
    static const char body[] =
        "--" B "\r\n"
        "X-Empty:\r\n"
        "Content-Disposition: form-data; name=\"named\"\r\n"
        "\r\n"
        "value\r\n"
        "--" B "--\r\n";

    cwist_multipart_result *r = cwist_multipart_parse(body, sizeof(body) - 1, B);
    assert(r != NULL);
    assert(count_fields(r) == 1);
    assert(r->fields->name != NULL);
    assert(strcmp(r->fields->name, "named") == 0);
    assert(r->fields->data_len == 5 && memcmp(r->fields->data, "value", 5) == 0);
    cwist_multipart_result_destroy(r);

    /* Same thing with a space after the colon, which the parser skips. */
    static const char body2[] =
        "--" B "\r\n"
        "X-Empty: \r\n"
        "Content-Disposition: form-data; name=\"named2\"\r\n"
        "\r\n"
        "v2\r\n"
        "--" B "--\r\n";
    r = cwist_multipart_parse(body2, sizeof(body2) - 1, B);
    assert(r != NULL);
    assert(count_fields(r) == 1);
    assert(r->fields->name && strcmp(r->fields->name, "named2") == 0);
    cwist_multipart_result_destroy(r);
    printf("  Passed.\n");
}

/* A body that ends before its closing boundary must not leak the in-flight
 * part buffer.  Completed parts are still returned; the truncated one is
 * dropped.  Run under ASan/LSan to observe the leak on unfixed code. */
static void test_truncated_body_drops_partial_part(void) {
    printf("Testing truncated body...\n");
    static const char body[] =
        "--" B "\r\n"
        "Content-Disposition: form-data; name=\"done\"\r\n"
        "\r\n"
        "complete\r\n"
        "--" B "\r\n"
        "Content-Disposition: form-data; name=\"cut\"\r\n"
        "\r\n"
        "this part never sees its closing boundary";

    cwist_multipart_result *r = cwist_multipart_parse(body, sizeof(body) - 1, B);
    assert(r != NULL);
    assert(count_fields(r) == 1);
    assert(find_field(r, "done") != NULL);
    assert(find_field(r, "cut") == NULL);
    cwist_multipart_result_destroy(r);

    /* Truncated inside the very first part: nothing completed, no leak. */
    static const char body2[] =
        "--" B "\r\n"
        "Content-Disposition: form-data; name=\"only\"\r\n"
        "\r\n"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    r = cwist_multipart_parse(body2, sizeof(body2) - 1, B);
    assert(r != NULL);
    assert(count_fields(r) == 0);
    cwist_multipart_result_destroy(r);
    printf("  Passed.\n");
}

/* Input the underlying parser rejects (an illegal byte in a header name) is
 * reported as NULL, per the documented contract, rather than as a result that
 * silently omits everything after the error. */
static void test_malformed_input_returns_null(void) {
    printf("Testing malformed input...\n");
    static const char body[] =
        "--" B "\r\n"
        "Bad Header Name: x\r\n"
        "\r\n"
        "data\r\n"
        "--" B "--\r\n";
    assert(cwist_multipart_parse(body, sizeof(body) - 1, B) == NULL);

    static const char garbage[] = "this is not multipart at all";
    assert(cwist_multipart_parse(garbage, sizeof(garbage) - 1, B) == NULL);
    printf("  Passed.\n");
}

static void test_extract_boundary(void) {
    printf("Testing boundary extraction...\n");
    char *b = cwist_multipart_extract_boundary("multipart/form-data; boundary=" B);
    assert(b && strcmp(b, B) == 0);
    free(b);
    b = cwist_multipart_extract_boundary("multipart/form-data; boundary=\"" B "\"");
    assert(b && strcmp(b, B) == 0);
    free(b);
    assert(cwist_multipart_extract_boundary("text/plain") == NULL);
    printf("  Passed.\n");
}

int main(void) {
    test_basic_two_fields();
    test_empty_header_value_does_not_swallow_next_header();
    test_truncated_body_drops_partial_part();
    test_malformed_input_returns_null();
    test_extract_boundary();
    printf("All multipart tests passed.\n");
    return 0;
}
