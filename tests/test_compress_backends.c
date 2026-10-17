/**
 * @file test_compress_backends.c
 * @brief Tests for the built-in deflate and zstd compression backends (issue #392).
 *
 * Covers:
 *   cwist_compress_backend_deflate() -- factory returns a valid descriptor.
 *   cwist_compress_backend_zstd()    -- factory returns a valid descriptor.
 *   Round-trip: init -> compress -> finish -> cleanup completes without error.
 *   cwist_compress_register_backend() + cwist_compress_unregister_all() -- smoke.
 *
 * The interface is defined in include/cwist/sys/app/compress.h.  Decompression
 * is not part of the backend interface, so the test only asserts that the
 * compress/finish pipeline produces non-empty output and returns 0.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/compress.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Output buffer shared by all round-trip tests. */
#define OUT_CAP 65536
static char out_buf[OUT_CAP];

static void test_descriptor(const cwist_compress_backend *b, const char *label) {
    printf("test_%s_descriptor...\n", label);

    assert(b != NULL);
    assert(b->encoding_name != NULL);
    assert(b->encoding_name[0] != '\0');
    assert(b->init    != NULL);
    assert(b->compress != NULL);
    assert(b->finish  != NULL);
    assert(b->cleanup != NULL);

    printf("  encoding_name=%s OK\n", b->encoding_name);
}

static void test_round_trip(const cwist_compress_backend *b, const char *label) {
    printf("test_%s_round_trip...\n", label);

    void *state = NULL;
    int rc = b->init(&state);
    assert(rc == 0);
    assert(state != NULL);

    const char input[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    size_t out_len = OUT_CAP;

    rc = b->compress(state, input, sizeof(input) - 1, out_buf, &out_len, 1 /* flush */);
    assert(rc == 0);

    size_t finish_len = OUT_CAP - out_len;
    rc = b->finish(state, out_buf + out_len, &finish_len);
    assert(rc == 0);

    size_t total = out_len + finish_len;
    assert(total > 0);

    b->cleanup(state);
    printf("  compressed %zu -> %zu bytes OK\n", sizeof(input) - 1, total);
}

static void test_distinct_names(void) {
    printf("test_distinct_encoding_names...\n");

    const cwist_compress_backend *deflate = cwist_compress_backend_deflate();
    const cwist_compress_backend *zstd    = cwist_compress_backend_zstd();

    assert(deflate != NULL && zstd != NULL);
    assert(strcmp(deflate->encoding_name, zstd->encoding_name) != 0);

    printf("  deflate=%s, zstd=%s OK\n",
           deflate->encoding_name, zstd->encoding_name);
}

static void test_register_unregister(void) {
    printf("test_compress_register_backend + unregister_all...\n");

    const cwist_compress_backend *b = cwist_compress_backend_deflate();
    assert(b != NULL);

    /* register must not crash. */
    cwist_compress_register_backend(b);

    /* unregister_all is idempotent -- calling twice must not crash. */
    cwist_compress_unregister_all();
    cwist_compress_unregister_all();

    printf("  OK\n");
}

int main(void) {
    const cwist_compress_backend *deflate = cwist_compress_backend_deflate();
    const cwist_compress_backend *zstd    = cwist_compress_backend_zstd();

    test_descriptor(deflate, "deflate");
    test_descriptor(zstd,    "zstd");

    test_round_trip(deflate, "deflate");
    test_round_trip(zstd,    "zstd");

    test_distinct_names();
    test_register_unregister();

    printf("All test_compress_backends tests passed.\n");
    return 0;
}
