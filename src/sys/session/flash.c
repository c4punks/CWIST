/** @file flash.c
 * @brief flash.c interface.
 */
#include <cwist/sys/session/flash.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

#include <cwist/core/siphash/siphash.h>

/** @brief Store a flash entry on the request.
 *
 * Creates the request's flash query map on first use. A NULL value is
 * stored as an empty string.
 *
 * @param req Request to attach the flash entry to; ignored if NULL.
 * @param key Entry key; ignored if NULL.
 * @param value Entry value; NULL is stored as "".
 */
void cwist_flash_set(cwist_http_request *req, const char *key, const char *value) {
    if (!req || !key) return;
    if (!req->flash) {
        req->flash = cwist_query_map_create();
    }
    if (req->flash) {
        cwist_query_map_set(req->flash, key, value ? value : "");
    }
}

/** @brief Read a flash entry without removing it.
 *
 * Non-destructive lookup; the entry remains in the flash map.
 *
 * @param req Request whose flash map is read.
 * @param key Entry key to look up.
 * @return Pointer to the stored value, or NULL if req/key are NULL or the
 *         key is not present. The pointer stays owned by the flash map.
 */
const char *cwist_flash_peek(cwist_http_request *req, const char *key) {
    if (!req || !key || !req->flash) return NULL;
    return cwist_query_map_get(req->flash, key);
}

/** @brief Read and remove a flash entry.
 *
 * Destructive lookup: on success the entry's bucket is unlinked and freed,
 * and ownership of the value string is transferred to the caller, who must
 * release it with cwist_free().
 *
 * @param req Request whose flash map is read.
 * @param key Entry key to look up.
 * @return Newly owned copy pointer to the value (caller frees), or NULL if
 *         req/key are NULL, the flash map is empty, or the key is missing.
 */
const char *cwist_flash_get(cwist_http_request *req, const char *key) {
    if (!req || !key || !req->flash || !req->flash->buckets || req->flash->size == 0) return NULL;
    uint64_t hash = siphash24(key, strlen(key), req->flash->seed);
    size_t idx = hash % req->flash->size;

    cwist_query_bucket **prev = &req->flash->buckets[idx];
    cwist_query_bucket *curr = *prev;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            *prev = curr->next;
            if (req->flash->count > 0) req->flash->count--;
            cwist_free(curr->key);
            /* ownership of value transfers to the caller, who must free it */
            char *ret_val = curr->value;
            cwist_free(curr);
            return ret_val;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    return NULL;
}

/** @brief Serialize all flash entries to a JSON object and clear the map.
 *
 * Builds a JSON object string with keys and values escaped by cJSON, then
 * frees every flash entry and empties the map.
 *
 * @param req Request whose flash map is serialized.
 * @return Newly allocated NUL-terminated JSON string the caller must free
 *         with cwist_free(), or NULL if there are no entries or if a buffer
 *         allocation fails (in which case the flash map is left unchanged).
 */
char *cwist_flash_pop_all_json(cwist_http_request *req) {
    if (!req || !req->flash || !req->flash->buckets || req->flash->size == 0 ||
        req->flash->count == 0)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    for (size_t i = 0; i < req->flash->size; i++) {
        for (cwist_query_bucket *b = req->flash->buckets[i]; b; b = b->next) {
            if (!cJSON_AddStringToObject(root, b->key, b->value)) {
                cJSON_Delete(root);
                return NULL;
            }
        }
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return NULL;
    char *json = cwist_strdup(printed);
    cJSON_free(printed);
    if (!json) return NULL;
    cwist_query_map_clear(req->flash);
    return json;
}
