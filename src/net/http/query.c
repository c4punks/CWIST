#define _POSIX_C_SOURCE 200809L
#include <cwist/query.h>
#include <cwist/siphash.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uriparser/Uri.h>

#define CWIST_QUERY_MAP_DEFAULT_SIZE 16

cwist_query_map *cwist_query_map_create(void) {
    cwist_query_map *map = (cwist_query_map *)malloc(sizeof(cwist_query_map));
    if (!map) return NULL;

    map->size = CWIST_QUERY_MAP_DEFAULT_SIZE;
    if (arena) {
        map->buckets = (cwist_query_bucket **)cwist_arena_alloc(
            (cwist_arena_t *)arena, map->size * sizeof(cwist_query_bucket *));
    } else {
        map->buckets =
            (cwist_query_bucket **)cwist_alloc_array(map->size, sizeof(cwist_query_bucket *));
    }
    if (!map->buckets) {
        free(map);
        return NULL;
    }

    cwist_generate_hash_seed(map->seed);
    return map;
}

void cwist_query_map_destroy(cwist_query_map *map) {
    if (!map) return;
    cwist_query_map_clear(map);
    free(map->buckets);
    free(map);
}

void cwist_query_map_clear(cwist_query_map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cwist_query_bucket *next = curr->next;
            free(curr->key);
            free(curr->value);
            free(curr);
            curr = next;
        }
        map->buckets[i] = NULL;
    }
}

void cwist_query_map_set(cwist_query_map *map, const char *key, const char *value) {
    if (!map || !key || !value) return;

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            // Update existing
            free(curr->value);
            curr->value = strdup(value);
            return;
        }
        curr = curr->next;
    }

    // Insert new
    cwist_query_bucket *node;
    if (map->arena) {
        node = (cwist_query_bucket *)cwist_arena_alloc((cwist_arena_t *)map->arena,
                                                       sizeof(cwist_query_bucket));
    } else {
        node = (cwist_query_bucket *)cwist_alloc(sizeof(cwist_query_bucket));
    }
    if (!node) return;
    node->key = strdup(key);
    node->value = strdup(value);
    node->next = map->buckets[index];
    map->buckets[index] = node;
}

const char *cwist_query_map_get(cwist_query_map *map, const char *key) {
    if (!map || !key) return NULL;

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

void cwist_query_map_delete(cwist_query_map *map, const char *key) {
    if (!map || !key) return;
    if (map->arena) {
        /* In an arena-managed map, deletion is handled as a no-op or lazy nullification to prevent
         * manual frees. */
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
                return;
            }
            prev = curr;
            curr = curr->next;
        }
        return;
    }

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
            cwist_free(curr->key);
            cwist_free(curr->value);
            cwist_free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

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

/**
 * @brief Decode a URL-encoded component with '+' → space semantics.
 *        '+' → 0x20, '%XY' → hex byte (strict), everything else passthrough.
 * @param src Raw (still encoded) key or value.
 * @return Newly allocated decoded string, or NULL on allocation failure.
 */
static char *url_decode(void *arena, const char *src) {
    if (!src) return NULL;

    size_t len = strlen(src);
    char *out;
    if (arena) {
        out = (char *)cwist_arena_alloc((cwist_arena_t *)arena, len + 1);
    } else {
        out = (char *)cwist_alloc(len + 1);
    }
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            unsigned int byte;
            sscanf(src + i + 1, "%2x", &byte);
            out[j++] = (char)byte;
            i += 2;
        } else if (src[i] == '+') {
            out[j++] = 0x20; /* SPACE (U+0020) */
        } else {
            out[j++] = src[i];
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

    UriQueryListA *queryList = NULL;
    int itemCount = 0;
    
    // uriparser handles & and = and url decoding
    if (uriDissectQueryMallocA(&queryList, &itemCount, raw_query, raw_query + strlen(raw_query)) != URI_SUCCESS) {
        return;
    }

    UriQueryListA *curr = queryList;
    while (curr) {
        if (curr->key) {
            cwist_query_map_set(map, curr->key, curr->value ? curr->value : "");
        }
        curr = curr->next;
    }

    uriFreeQueryListA(queryList);
}
