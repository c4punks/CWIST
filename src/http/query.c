#define _POSIX_C_SOURCE 200809L
#include <cwist/query.h>
#include <cwist/siphash.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CWIST_QUERY_MAP_DEFAULT_SIZE 16

cwist_query_map *cwist_query_map_create(void) {
    cwist_query_map *map = (cwist_query_map *)malloc(sizeof(cwist_query_map));
    if (!map) return NULL;

    map->size = CWIST_QUERY_MAP_DEFAULT_SIZE;
    map->buckets = (cwist_query_bucket **)calloc(map->size, sizeof(cwist_query_bucket *));
    if (!map->buckets) {
        free(map);
        return NULL;
    }

    cwist_generate_hash_seed(map->seed);
    return map;
}

void cwist_query_map_destroy(cwist_query_map *map) {
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
    }
    free(map->buckets);
    free(map);
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
    cwist_query_bucket *node = (cwist_query_bucket *)malloc(sizeof(cwist_query_bucket));
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

void cwist_query_map_parse(cwist_query_map *map, const char *raw_query) {
    if (!map || !raw_query || strlen(raw_query) == 0) return;

    char *query_dup = strdup(raw_query);
    if (!query_dup) return;

    char *pair_token;
    char *pair_rest = query_dup;

    while ((pair_token = strtok_r(pair_rest, "&", &pair_rest))) {
        char *eq = strchr(pair_token, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair_token;
            const char *value = eq + 1;
            cwist_query_map_set(map, key, value);
        } else {
            // Flag style: "active" -> key="active", value=""
            cwist_query_map_set(map, pair_token, "");
        }
    }

    free(query_dup);
}
