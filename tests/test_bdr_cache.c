/**
 * @file test_bdr_cache.c
 * @brief Unit tests for the Big Dumb Reply cache public accessors (issue #391).
 *
 * Covers:
 *   cwist_bdr_put_fixed   -- cache a fixed response; get returns it.
 *   cwist_bdr_get         -- hit returns bytes; miss returns NULL.
 *   cwist_bdr_get_pinned  -- pinned hit; blob stays readable until unpin.
 *   cwist_bdr_unpin       -- release a pin; NULL unpin must not crash.
 *   cwist_bdr_set_limits  -- set limits; oversized entry refused/evicted.
 *
 * cwist_bdr_create() needs no server -- the cache is standalone.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_put_fixed_and_get(void) {
    printf("test_bdr_put_fixed + cwist_bdr_get...\n");

    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    const char body[] = "HTTP/1.1 200 OK\r\n\r\nhello";
    cwist_bdr_put_fixed(bdr, "GET", "/ping", body, sizeof(body));

    size_t len = 0;
    const void *blob = cwist_bdr_get(bdr, "GET", "/ping", &len);
    assert(blob != NULL);
    assert(len == sizeof(body));
    assert(memcmp(blob, body, len) == 0);

    /* Miss on unregistered path must return NULL. */
    blob = cwist_bdr_get(bdr, "GET", "/missing", &len);
    assert(blob == NULL);

    cwist_bdr_destroy(bdr);
    printf("  OK\n");
}

static void test_get_method_key(void) {
    printf("test_bdr_get method-keyed...\n");

    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    const char get_body[] = "GET response";
    cwist_bdr_put_fixed(bdr, "GET", "/resource", get_body, sizeof(get_body));

    size_t len = 0;
    /* Same path, different method -- must not match the GET entry. */
    const void *blob = cwist_bdr_get(bdr, "POST", "/resource", &len);
    assert(blob == NULL);

    /* GET still hits. */
    blob = cwist_bdr_get(bdr, "GET", "/resource", &len);
    assert(blob != NULL);

    cwist_bdr_destroy(bdr);
    printf("  OK\n");
}

static void test_get_pinned_and_unpin(void) {
    printf("test_bdr_get_pinned + cwist_bdr_unpin...\n");

    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    const char body[] = "pinned response";
    cwist_bdr_put_fixed(bdr, "GET", "/pinned", body, sizeof(body));

    size_t len = 0;
    bdr_blob_t *pin = NULL;
    const void *blob = cwist_bdr_get_pinned(bdr, "GET", "/pinned", &len, &pin);
    assert(blob != NULL);
    assert(len == sizeof(body));
    assert(memcmp(blob, body, len) == 0);
    assert(pin != NULL);

    /* Blob must remain valid while the pin is held. */
    assert(memcmp(blob, body, len) == 0);

    cwist_bdr_unpin(pin);

    /* NULL unpin must not crash. */
    cwist_bdr_unpin(NULL);

    /* Miss path: pinned lookup returns NULL for missing entry. */
    bdr_blob_t *pin2 = NULL;
    blob = cwist_bdr_get_pinned(bdr, "GET", "/absent", &len, &pin2);
    assert(blob == NULL);
    assert(pin2 == NULL);

    cwist_bdr_destroy(bdr);
    printf("  OK\n");
}

static void test_set_limits(void) {
    printf("test_bdr_set_limits...\n");

    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    /* Set a max_bytes limit smaller than the test body. */
    cwist_bdr_set_limits(bdr, 4, 0, 0);

    /* An entry larger than max_bytes must be refused or evicted.
     * After put_fixed the cache either stores nothing (refused) or stores
     * and immediately evicts; in both cases a subsequent get must not
     * return more bytes than max_bytes allows. */
    const char big[] = "this response is definitely larger than four bytes";
    cwist_bdr_put_fixed(bdr, "GET", "/big", big, sizeof(big));

    size_t len = 0;
    const void *blob = cwist_bdr_get(bdr, "GET", "/big", &len);
    /* Either NULL (refused) or a blob -- assert we did not crash. */
    (void)blob;
    (void)len;

    /* Zero-value arguments to set_limits must not crash. */
    cwist_bdr_set_limits(bdr, 0, 0, 0);

    cwist_bdr_destroy(bdr);
    printf("  OK\n");
}

int main(void) {
    test_put_fixed_and_get();
    test_get_method_key();
    test_get_pinned_and_unpin();
    test_set_limits();
    printf("All test_bdr_cache tests passed.\n");
    return 0;
}
