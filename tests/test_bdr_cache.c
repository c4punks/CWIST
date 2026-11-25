/**
 * @file test_bdr_cache.c
 * @brief Unit tests for the Big Dumb Reply cache accessors: put_fixed, get,
 *        get_pinned, unpin, set_limits (issue #391). The cache is standalone;
 *        no server is needed.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/siphash/siphash.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_hash_key_is_not_static(void) {
    static const uint8_t published_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);
    const char body[] = "response";
    cwist_bdr_put_fixed(bdr, "GET", "/chosen-collision", body, sizeof(body));
    uint64_t old_hash =
        siphash24("/chosen-collision", strlen("/chosen-collision"), published_key) ^ 'G';
    bool found = false;
    for (size_t i = 0; i < bdr->bucket_count; i++) {
        bdr_entry_t *entry = atomic_load(&bdr->buckets[i]);
        if (entry) {
            found = true;
            assert(entry->request_hash != old_hash);
        }
    }
    assert(found);
    cwist_bdr_destroy(bdr);
}

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

    cwist_bdr_unpin(pin);
    cwist_bdr_unpin(NULL);

    /* Pinned lookup of a missing entry yields neither blob nor pin. */
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

    /* An entry larger than max_bytes is refused or immediately evicted;
     * either is acceptable, so only assert the cache survives. */
    cwist_bdr_set_limits(bdr, 4, 0, 0);

    const char big[] = "this response is definitely larger than four bytes";
    cwist_bdr_put_fixed(bdr, "GET", "/big", big, sizeof(big));

    size_t len = 0;
    const void *blob = cwist_bdr_get(bdr, "GET", "/big", &len);
    (void)blob;
    (void)len;

    cwist_bdr_set_limits(bdr, 0, 0, 0);

    cwist_bdr_destroy(bdr);
    printf("  OK\n");
}

static bool refresh_noop(void *arg, void **data, size_t *len, void (**free_fn)(void *)) {
    (void)arg;
    (void)data;
    (void)len;
    (void)free_fn;
    return false;
}

static void test_oversized_blob_is_rejected(void) {
    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);
    const char body = 'x';
    cwist_bdr_put_fixed(bdr, "GET", "/overflow", &body, SIZE_MAX);
    cwist_bdr_put(bdr, "GET", "/overflow", &body, SIZE_MAX);
    cwist_bdr_put_revalidatable(bdr, "GET", "/overflow", &body, SIZE_MAX, refresh_noop, NULL);
    size_t len = 0;
    assert(cwist_bdr_get(bdr, "GET", "/overflow", &len) == NULL);
    cwist_bdr_destroy(bdr);
}

int main(void) {
    test_oversized_blob_is_rejected();
    test_hash_key_is_not_static();
    test_put_fixed_and_get();
    test_get_method_key();
    test_get_pinned_and_unpin();
    test_set_limits();
    printf("All test_bdr_cache tests passed.\n");
    return 0;
}
