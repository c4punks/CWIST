#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/big_dumb_reply.h>

typedef struct {
    cwist_bdr_t *bdr;
    const char *path;
    const char *body;
} bdr_worker_args;

static void *bdr_worker(void *opaque) {
    bdr_worker_args *args = opaque;
    for (int i = 0; i < 1000; ++i) {
        size_t len = 0;
        cwist_bdr_put(args->bdr, "GET", args->path, args->body, strlen(args->body) + 1);
        char *copy = cwist_bdr_copy_get(args->bdr, "GET", args->path, &len);
        if (copy) {
            assert(len == strlen(args->body) + 1);
            assert(strcmp(copy, args->body) == 0);
            cwist_free(copy);
        }
    }
    return NULL;
}

int main(void) {
    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    const char root[] = "root response";
    const char posts[] = "posts response";
    size_t len = 0;

    /* Two matching observations promote each path independently. */
    cwist_bdr_put(bdr, "GET", "/", root, sizeof(root));
    cwist_bdr_put(bdr, "GET", "/", root, sizeof(root));
    cwist_bdr_put(bdr, "GET", "/posts", posts, sizeof(posts));
    cwist_bdr_put(bdr, "GET", "/posts", posts, sizeof(posts));

    char *copy = cwist_bdr_copy_get(bdr, "GET", "/posts", &len);
    assert(copy != NULL);
    assert(len == sizeof(posts));
    assert(memcmp(copy, posts, len) == 0);

    /* A cache update may retire the internal root blob, never this copy. */
    cwist_bdr_put(bdr, "GET", "/", "new root", sizeof("new root"));
    assert(memcmp(copy, posts, len) == 0);
    cwist_free(copy);

    assert(cwist_bdr_copy_get(bdr, "GET", "/missing", &len) == NULL);

    bdr_worker_args left = {bdr, "/left", "left response"};
    bdr_worker_args right = {bdr, "/right", "right response"};
    pthread_t left_thread;
    pthread_t right_thread;
    assert(pthread_create(&left_thread, NULL, bdr_worker, &left) == 0);
    assert(pthread_create(&right_thread, NULL, bdr_worker, &right) == 0);
    assert(pthread_join(left_thread, NULL) == 0);
    assert(pthread_join(right_thread, NULL) == 0);

    /* --- Per-connection cursor fast path --- */
    cwist_bdr_cursor_t cursor = {0};
    bdr_blob_t *pin = NULL;

    /* Warm both routes through the plain API, then serve via the cursor. */
    cwist_bdr_put(bdr, "GET", "/cursor-a", "alpha body", sizeof("alpha body"));
    cwist_bdr_put(bdr, "GET", "/cursor-a", "alpha body", sizeof("alpha body"));
    cwist_bdr_put(bdr, "GET", "/cursor-b", "bravo body", sizeof("bravo body"));
    cwist_bdr_put(bdr, "GET", "/cursor-b", "bravo body", sizeof("bravo body"));

    const void *data = cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor-a", 9, &len, &pin, &cursor);
    assert(data != NULL && pin != NULL);
    assert(len == sizeof("alpha body"));
    assert(memcmp(data, "alpha body", len) == 0);
    cwist_bdr_unpin(pin);

    /* Repeated hit on the same connection: content-compare path. */
    for (int i = 0; i < 100; ++i) {
        data = cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor-a", 9, &len, &pin, &cursor);
        assert(data != NULL && pin != NULL);
        assert(len == sizeof("alpha body"));
        assert(memcmp(data, "alpha body", len) == 0);
        cwist_bdr_unpin(pin);
    }

    /* Different route on the same connection: cursor swaps, still correct. */
    data = cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor-b", 9, &len, &pin, &cursor);
    assert(data != NULL && pin != NULL);
    assert(len == sizeof("bravo body"));
    assert(memcmp(data, "bravo body", len) == 0);
    cwist_bdr_unpin(pin);

    /* Prefix collision guard: "/cursor" must not match the "/cursor-a" slot. */
    pin = NULL;
    assert(cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor", 7, &len, &pin, &cursor) == NULL);
    assert(pin == NULL);

    /* Content change under the cursor: the stale hint must miss, not serve
     * the old bytes. Demote then restabilize with new content. */
    cwist_bdr_put(bdr, "GET", "/cursor-b", "bravo CHANGED", sizeof("bravo CHANGED"));
    pin = NULL;
    assert(cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor-b", 9, &len, &pin, &cursor) == NULL);
    cwist_bdr_put(bdr, "GET", "/cursor-b", "bravo CHANGED", sizeof("bravo CHANGED"));
    data = cwist_bdr_get_pinned_cursor(bdr, "GET", "/cursor-b", 9, &len, &pin, &cursor);
    assert(data != NULL && pin != NULL);
    assert(len == sizeof("bravo CHANGED"));
    assert(memcmp(data, "bravo CHANGED", len) == 0);
    cwist_bdr_unpin(pin);

    /* Non-GET and missing routes never hit. */
    pin = NULL;
    assert(cwist_bdr_get_pinned_cursor(bdr, "POST", "/cursor-b", 9, &len, &pin, &cursor) == NULL);
    pin = NULL;
    assert(cwist_bdr_get_pinned_cursor(bdr, "GET", "/nope", 5, &len, &pin, &cursor) == NULL);

    cwist_bdr_destroy(bdr);
    puts("test_bdr: OK");
    return 0;
}
