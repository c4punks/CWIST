#include <cwist/core/mem/arena.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_default_buffers_are_recycled(void) {
    cwist_arena_t *a = cwist_arena_create(0);
    cwist_arena_t *b = cwist_arena_create(0);
    assert(a && b);
    void *pa = cwist_arena_alloc(a, 1);
    void *pb = cwist_arena_alloc(b, 1);
    assert(pa && pb && pa != pb);
    cwist_arena_destroy(a);
    cwist_arena_destroy(b);

    a = cwist_arena_create(0);
    b = cwist_arena_create(0);
    assert(a && b);
    void *qa = cwist_arena_alloc(a, 1);
    void *qb = cwist_arena_alloc(b, 1);
    assert(qa && qb);
    assert((qa == pa && qb == pb) || (qa == pb && qb == pa));
    cwist_arena_destroy(a);
    cwist_arena_destroy(b);
}

static void test_small_and_custom_arenas(void) {
    cwist_arena_t *arena = cwist_arena_create(32);
    assert(arena);
    assert(cwist_arena_alloc(arena, 16));
    assert(cwist_arena_alloc(arena, 16));
    assert(cwist_arena_alloc(arena, 1) == NULL);
    assert(cwist_arena_alloc(arena, SIZE_MAX) == NULL);
    cwist_arena_destroy(arena);

    arena = cwist_arena_create(CWIST_ARENA_DEFAULT_GENERATION_BYTES + 16);
    assert(arena);
    assert(cwist_arena_alloc(arena, CWIST_ARENA_DEFAULT_GENERATION_BYTES));
    cwist_arena_destroy(arena);
}

int main(void) {
    test_default_buffers_are_recycled();
    test_small_and_custom_arenas();
    puts("test_baremetal_arena: PASS");
    return 0;
}
