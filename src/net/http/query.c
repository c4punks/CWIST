#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/query.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/arena.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/**
 * @file query.c
 * @brief Hash-map based storage and parsing helpers for URL query parameters.
 */

#define CWIST_QUERY_MAP_DEFAULT_SIZE 16

/**
 * @brief Duplicate a string using the map's allocator.
 * @param arena Memory arena to allocate from, or NULL to use heap allocation.
 * @param str Source string; NULL yields NULL.
 * @return Newly allocated copy owned by the arena or heap, or NULL on failure.
 */
/* Arena maps carve from the request arena, which is a fixed 8 KiB
 * generation: a query of a few KiB used to exhaust it, and every later
 * key/value was dropped silently. Allocations now fall back to the heap,
 * and every release path frees whatever the arena does not own. */
static void *query_alloc(void *arena, size_t size) {
    if (arena) {
        void *p = cwist_arena_alloc((cwist_arena_t *)arena, size);
        if (p) return p;
    }
    return cwist_alloc(size);
}

static void query_release(void *arena, void *ptr) {
    if (!ptr) return;
    if (arena && cwist_arena_owns((cwist_arena_t *)arena, ptr)) return;
    cwist_free(ptr);
}

static char *query_strdup(void *arena, const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = (char *)query_alloc(arena, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

/**
 * @brief Allocate a query map and seed its SipHash key material.
 * @return Newly allocated map, or NULL when memory allocation fails.
 */
cwist_query_map *cwist_query_map_create(void) {
    return cwist_query_map_create_in_arena(NULL);
}

/**
 * @brief Allocate a query map within a memory arena to avoid GC/heap allocations.
 */
cwist_query_map *cwist_query_map_create_in_arena(void *arena) {
    cwist_query_map *map = (cwist_query_map *)query_alloc(arena, sizeof(cwist_query_map));
    if (!map) return NULL;
    map->size = CWIST_QUERY_MAP_DEFAULT_SIZE;
    map->buckets = (cwist_query_bucket **)query_alloc(arena, map->size * sizeof(cwist_query_bucket *));
    if (!map->buckets) {
        query_release(arena, map);
        return NULL;
    }
    memset(map->buckets, 0, map->size * sizeof(cwist_query_bucket *));
    map->arena = arena;
    map->count = 0;
    cwist_generate_hash_seed(map->seed);
    return map;
}

/**
 * @brief Release every bucket node and the map container itself.
 * @param map Query map to destroy.
 */
void cwist_query_map_destroy(cwist_query_map *map) {
    if (!map) return;
    void *arena = map->arena;
    cwist_query_map_clear(map);
    query_release(arena, map->buckets);
    query_release(arena, map);
}

/**
 * @brief Remove every key/value pair while keeping the bucket array allocated.
 * @param map Query map to clear in-place.
 */
void cwist_query_map_clear(cwist_query_map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cwist_query_bucket *next = curr->next;
            query_release(map->arena, curr->key);
            query_release(map->arena, curr->value);
            query_release(map->arena, curr);
            curr = next;
        }
        map->buckets[i] = NULL;
    }
    map->count = 0;
}

/**
 * @brief Double the bucket array once the load factor passes 3/4.
 *
 * The map used to keep its initial 16 buckets forever, so a request with
 * hundreds of parameters walked chains of dozens of nodes on every lookup.
 * Nodes are relinked, not copied; on allocation failure the map keeps its
 * current size and stays correct, only slower.
 */
static void query_map_grow(cwist_query_map *map) {
    if (map->size && map->count + 1 <= map->size - map->size / 4) return;
    size_t new_size = map->size ? map->size * 2 : CWIST_QUERY_MAP_DEFAULT_SIZE;
    if (new_size <= map->size || new_size > SIZE_MAX / sizeof(cwist_query_bucket *)) return;
    cwist_query_bucket **buckets =
        (cwist_query_bucket **)query_alloc(map->arena, new_size * sizeof(cwist_query_bucket *));
    if (!buckets) return;
    memset(buckets, 0, new_size * sizeof(cwist_query_bucket *));
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cwist_query_bucket *next = curr->next;
            size_t index = siphash24(curr->key, strlen(curr->key), map->seed) % new_size;
            curr->next = buckets[index];
            buckets[index] = curr;
            curr = next;
        }
    }
    query_release(map->arena, map->buckets);
    map->buckets = buckets;
    map->size = new_size;
}

/**
 * @brief Insert or replace a decoded query parameter in the map.
 * @param map Query map that owns the entry storage.
 * @param key Decoded query-string key.
 * @param value Decoded query-string value.
 */
void cwist_query_map_set(cwist_query_map *map, const char *key, const char *value) {
    if (!map || !key || !value) return;
    if (!map->size) {
        query_map_grow(map);
        if (!map->size) return;
    }

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            // Update existing; keep the old value if the copy fails
            char *copy = query_strdup(map->arena, value);
            if (!copy) return;
            query_release(map->arena, curr->value);
            curr->value = copy;
            return;
        }
        curr = curr->next;
    }

    // Insert new
    if (map->count + 1 > map->size - map->size / 4) {
        query_map_grow(map);
        index = hash % map->size;
    }
    cwist_query_bucket *node = (cwist_query_bucket *)query_alloc(map->arena, sizeof(cwist_query_bucket));
    if (!node) return;
    node->key = query_strdup(map->arena, key);
    node->value = query_strdup(map->arena, value);
    if (!node->key || !node->value) {
        /* A NULL key in a bucket would crash the next strcmp(). */
        query_release(map->arena, node->key);
        query_release(map->arena, node->value);
        query_release(map->arena, node);
        return;
    }
    node->next = map->buckets[index];
    map->buckets[index] = node;
    map->count++;
}

/**
 * @brief Look up a decoded query parameter by key.
 * @param map Query map to search.
 * @param key Decoded key to retrieve.
 * @return Stored value, or NULL when the key is absent.
 */
const char *cwist_query_map_get(cwist_query_map *map, const char *key) {
    if (!map || !key || !map->size) return NULL;

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            return curr->value;
        }
        curr = curr->next;
    }
    return NULL;
}

/**
 * @brief Remove a key/value pair from the map if present.
 * @param map Query map to modify; NULL is a no-op.
 * @param key Key to delete; NULL is a no-op.
 * @retval In heap-managed maps the node and its strings are freed.
 * @retval In arena-managed maps the node is only unlinked; storage is reclaimed with the arena.
 *        (Nothing is freed.)
 */
void cwist_query_map_delete(cwist_query_map *map, const char *key) {
    if (!map || !key || !map->size) return;
    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    cwist_query_bucket *prev = NULL;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                map->buckets[index] = curr->next;
            }
            query_release(map->arena, curr->key);
            query_release(map->arena, curr->value);
            query_release(map->arena, curr);
            map->count--;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/**
 * @brief Invoke a callback for every key/value pair stored in the map.
 * @param map Query map to iterate; NULL is a no-op.
 * @param cb Callback invoked as cb(key, value, ctx) for each entry; NULL is a no-op.
 * @param ctx Opaque pointer forwarded to the callback for each entry.
 * @note Iteration order is bucket order and is not sorted.
 */
void cwist_query_map_foreach(cwist_query_map *map, cwist_query_map_iter_func cb, void *ctx) {
    if (!map || !cb) return;
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cb(curr->key, curr->value, ctx);
            curr = curr->next;
        }
    }
}

/* Hex nibble values for URL percent-decoding. Only consulted after
 * isxdigit validation, so non-hex entries are never read. */
static const uint8_t url_decode_hex[256] = {
    ['0'] = 0,  1,          2,          3,          4,          5,          6,          7,
    ['8'] = 8,  9,          ['A'] = 10, 11,         12,         13,         14,         15,
    ['a'] = 10, 11,         12,         13,         14,         15,
};

/**
 * @brief Decode a URL-encoded component with '+' → space semantics.
 *        '+' → 0x20, '%XY' → hex byte (strict), everything else passthrough.
 *        A '%' not followed by two hex digits is copied literally, matching
 *        the previous sscanf/isxdigit behavior.
 * @param src Raw (still encoded) key or value.
 * @return Newly allocated decoded string, or NULL on allocation failure.
 */
static char *url_decode(void *arena, const char *src) {
    if (!src) return NULL;

    size_t len = strlen(src);
    char *out = (char *)query_alloc(arena, len + 1);
    if (!out) return NULL;

    size_t j = 0;
    size_t i = 0;
    while (i < len) {
        /* Copy the plain run up to the next '%' or '+' in one shot. */
        size_t plain = strcspn(src + i, "%+");
        if (plain > 0) {
            memcpy(out + j, src + i, plain);
            j += plain;
            i += plain;
            if (i >= len) break;
        }
        if (src[i] == '+') {
            out[j++] = 0x20; /* SPACE (U+0020) */
            i++;
        } else if (i + 2 < len && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            out[j++] = (char)((url_decode_hex[(unsigned char)src[i + 1]] << 4) |
                              url_decode_hex[(unsigned char)src[i + 2]]);
            i += 3;
        } else {
            out[j++] = src[i];
            i++;
        }
    }
    out[j] = '\0';
    return out;
}

/**
 * @brief Parse a raw `a=1&b=2` query string into the existing map.
 * @param map Destination map that receives decoded keys and values.
 * @param raw_query Raw query substring without the leading question mark.
 */
void cwist_query_map_parse(cwist_query_map *map, const char *raw_query) {
    if (!map || !raw_query || strlen(raw_query) == 0) return;

    char *buffer = query_strdup(map->arena, raw_query);
    if (!buffer) return;

    /* Single pass over buffer splitting on '&' and '='; strtok_r's empty
     * token collapsing is reproduced by skipping zero-length segments. */
    char *seg = buffer;
    char *end = buffer + strlen(buffer);
    while (seg < end) {
        char *amp = memchr(seg, '&', (size_t)(end - seg));
        char *seg_end = amp ? amp : end;
        if (amp) *amp = '\0';

        char *token = seg;
        seg = seg_end + (amp ? 1 : 0);

        if (seg_end == token) continue; /* strtok_r skips empty tokens */

        char *eq = memchr(token, '=', (size_t)(seg_end - token));
        if (!eq) continue;

        *eq = '\0';
        const char *key_raw = token;
        const char *value_raw = eq + 1;

        char *key_dec = url_decode(map->arena, key_raw);
        char *value_dec = url_decode(map->arena, value_raw);

        if (key_dec && cwist_query_map_get(map, key_dec) == NULL) {
            cwist_query_map_set(map, key_dec, value_dec ? value_dec : "");
        }

        query_release(map->arena, key_dec);
        query_release(map->arena, value_dec);
    }

    query_release(map->arena, buffer);
}
