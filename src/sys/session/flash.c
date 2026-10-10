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
 * Builds a JSON object string {"key":"value",...} with '"' and '\' escaped
 * in keys and values, then frees every flash entry and empties the map.
 *
 * @param req Request whose flash map is serialized.
 * @return Newly allocated NUL-terminated JSON string the caller must free
 *         with cwist_free(), or NULL if there are no entries or if a buffer
 *         allocation fails (in which case the flash map is left unchanged).
 */
char *cwist_flash_pop_all_json(cwist_http_request *req) {
    if (!req || !req->flash || !req->flash->buckets || req->flash->size == 0) return NULL;
    int count = 0;
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            count++;
            b = b->next;
        }
    }
    if (count == 0) return NULL;

    size_t cap = 256;
    char *buf = (char *)cwist_alloc(cap);
    if (!buf) return NULL;
    size_t len = 0;
    buf[len++] = '{';
    int first = 1;
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            if (!first) {
                if (len + 2 >= cap) {
                    size_t new_cap = cap * 2;
                    char *new_buf = (char *)cwist_realloc(buf, new_cap);
                    if (!new_buf) {
                        cwist_free(buf);
                        return NULL;
                    }
                    buf = new_buf;
                    cap = new_cap;
                }
                buf[len++] = ',';
            }
            first = 0;
            size_t key_escaped_len = strlen(b->key) * 2 + 3;
            size_t val_escaped_len = strlen(b->value) * 2 + 3;
            size_t needed = key_escaped_len + val_escaped_len + 4;
            if (len + needed >= cap) {
                size_t new_cap = cap;
                while (len + needed >= new_cap) new_cap *= 2;
                char *new_buf = (char *)cwist_realloc(buf, new_cap);
                if (!new_buf) {
                    cwist_free(buf);
                    return NULL;
                }
                buf = new_buf;
                cap = new_cap;
            }
            buf[len++] = '"';
            for (char *p = b->key; *p; p++) {
                if (*p == '"' || *p == '\\') buf[len++] = '\\';
                buf[len++] = *p;
            }
            buf[len++] = '"';
            buf[len++] = ':';
            buf[len++] = '"';
            for (char *p = b->value; *p; p++) {
                if (*p == '"' || *p == '\\') buf[len++] = '\\';
                buf[len++] = *p;
            }
            buf[len++] = '"';
            b = b->next;
        }
    }
    if (len + 2 >= cap) {
        size_t new_cap = cap * 2;
        char *new_buf = (char *)cwist_realloc(buf, new_cap);
        if (!new_buf) {
            cwist_free(buf);
            return NULL;
        }
        buf = new_buf;
        cap = new_cap;
    }
    buf[len++] = '}';
    buf[len] = '\0';
    // Clear all flash entries
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            cwist_query_bucket *next = b->next;
            cwist_free(b->key);
            cwist_free(b->value);
            cwist_free(b);
            b = next;
        }
        req->flash->buckets[i] = NULL;
    }
    return buf;
}
