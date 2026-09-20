#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * @file big_dumb_reply.c
 * @brief Opportunistic response cache that stores stable GET replies in memory or on disk.
 */

#define BDR_BUCKETS 1024

/**
 * @brief Static SipHash key used to bucket request and response fingerprints.
 */
static const uint8_t BDR_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/**
 * @brief Free a cached response blob and debit its size from the cache budget.
 * @param bdr Cache instance that owns the blob.
 * @param entry Cache entry whose blob should be released.
 */
static void bdr_release_blob(cwist_bdr_t *bdr, bdr_entry_t *entry) {
    if (!entry || !entry->response_blob) return;
    if (bdr) {
        if (bdr->current_bytes >= entry->len) {
            bdr->current_bytes -= entry->len;
        } else {
            bdr->current_bytes = 0;
        }
    }
    cwist_free(entry->response_blob);
    entry->response_blob = NULL;
    entry->len = 0;
}

/**
 * @brief Remove one cache entry from its hash bucket and free its payload.
 * @param bdr Cache instance that owns the bucket array.
 * @param idx Bucket index that contains the entry.
 * @param prev Previous node in the chain, or NULL when removing the head.
 * @param entry Entry to remove.
 */
static void bdr_remove_entry(cwist_bdr_t *bdr, size_t idx, bdr_entry_t *prev, bdr_entry_t *entry) {
    if (!bdr || !entry || idx >= bdr->bucket_count) return;
    bdr_release_blob(bdr, entry);
    if (prev) {
        prev->next = entry->next;
    } else {
        bdr->buckets[idx] = entry->next;
    }
    cwist_free(entry);
}

/* --- Lookup helpers ------------------------------------------------------- */

static uint64_t bdr_hash(const char *method, const char *path) {
    uint64_t h = siphash24((const void *)path, strlen(path), BDR_KEY);
    h ^= (uint64_t)(method[0]);
    return h;
}

static uint64_t bdr_hash_n(const char *method, const char *path, size_t path_len) {
    uint64_t h = siphash24((const void *)path, path_len, BDR_KEY);
    h ^= (uint64_t)(method[0]);
    return h;
}

static uint64_t bdr_hash_data(const void *data, size_t len) {
    return siphash24(data, len, BDR_KEY);
}

static bool bdr_entry_should_decay(const cwist_bdr_t *bdr, const bdr_entry_t *entry, time_t now) {
    if (!bdr || !entry) return false;
    if (bdr->max_entry_age_sec > 0 && entry->created_at > 0) {
        if (now - entry->created_at > bdr->max_entry_age_sec) {
            return true;
        }
    }
    if (entry->is_stable && bdr->revalidate_hits > 0 && entry->hits >= bdr->revalidate_hits) {
        return true;
    }
    return false;
}

/**
 * @brief Evict the oldest cached entry to bring memory usage back under budget.
 * @param bdr Cache instance to trim.
 * @param now Current wall-clock time.
 * @return true when an entry was removed.
 */
static bool bdr_trim_oldest(cwist_bdr_t *bdr, time_t now) {
    if (!bdr) return false;
    size_t victim_idx = SIZE_MAX;
    bdr_entry_t *victim = NULL;
    bdr_entry_t *victim_prev = NULL;
    time_t oldest = now;

    for (size_t i = 0; i < bdr->bucket_count; ++i) {
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            if (curr->response_blob && (!victim || curr->created_at < oldest)) {
                victim = curr;
                victim_prev = prev;
                victim_idx = i;
                oldest = curr->created_at;
            }
            prev = curr;
            curr = curr->next;
        }
    }

    if (!victim || victim_idx == SIZE_MAX) return false;
    bdr_remove_entry(bdr, victim_idx, victim_prev, victim);
    return true;
}

/**
 * @brief Sweep a bounded number of buckets looking for stale cache entries.
 * @param bdr Cache instance to sweep.
 * @param steps Number of buckets to inspect this round.
 */
static void bdr_sweep(cwist_bdr_t *bdr, size_t steps) {
    if (!bdr || bdr->bucket_count == 0 || steps == 0) return;
    time_t now = time(NULL);
    for (size_t i = 0; i < steps; ++i) {
        size_t idx = bdr->gc_cursor % bdr->bucket_count;
        bdr->gc_cursor = (bdr->gc_cursor + 1) % bdr->bucket_count;
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = bdr->buckets[idx];
        while (curr) {
            if (bdr_entry_should_decay(bdr, curr, now)) {
                bdr_entry_t *victim = curr;
                curr = curr->next;
                bdr_remove_entry(bdr, idx, prev, victim);
                continue;
            }
            prev = curr;
            curr = curr->next;
        }
    }
}

/**
 * @brief Apply cache GC and size guardrails after inserts or disk-mode transitions.
 * @param bdr Cache instance to constrain.
 */
static void bdr_guardrails(cwist_bdr_t *bdr) {
    if (!bdr) return;
    bdr_sweep(bdr, BDR_GC_SWEEP);

    if (bdr->max_bytes == 0) return;
    time_t now = time(NULL);
    while (bdr->current_bytes > bdr->max_bytes) {
        if (!bdr_trim_oldest(bdr, now)) break;
    }
}

/**
 * @brief Allocate and initialize the Big Dumb Reply cache.
 * @return Newly allocated cache object, or NULL on allocation failure.
 */
cwist_bdr_t *cwist_bdr_create(void) {
    cwist_bdr_t *bdr = calloc(1, sizeof(cwist_bdr_t));
    if (!bdr) return NULL;
    if (pthread_mutex_init(&bdr->lock, NULL) != 0) {
        cwist_free(bdr);
        return NULL;
    }
    bdr->bucket_count = BDR_BUCKETS;
    bdr->buckets = cwist_alloc_array(BDR_BUCKETS, sizeof(bdr_entry_t *));
    if (!bdr->buckets) {
        pthread_mutex_destroy(&bdr->lock);
        cwist_free(bdr);
        return NULL;
    }
    bdr->latency_threshold_ms = 10; 
    bdr->disk_db = NULL;
    bdr->is_disk_mode = false;
    return bdr;
}

/**
 * @brief Destroy the cache and every response blob it currently owns.
 * @param bdr Cache instance to destroy.
 */
void cwist_bdr_destroy(cwist_bdr_t *bdr) {
    if (!bdr) return;
    pthread_mutex_lock(&bdr->lock);
    for (size_t i = 0; i < bdr->bucket_count; i++) {
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            bdr_entry_t *next = curr->next;
            free(curr->response_blob);
            free(curr);
            curr = next;
        }
    }
    free(bdr->buckets);
    if (bdr->disk_db) {
        sqlite3_close(bdr->disk_db);
        remove("cwist_bdr_fallback.db"); // Cleanup temp db
    }
    pthread_mutex_unlock(&bdr->lock);
    pthread_mutex_destroy(&bdr->lock);
    cwist_free(bdr);
}

/* --- Read path ------------------------------------------------------------ */

/* Shared read core: epoch-protected hit validation for a precomputed
 * request hash, optionally starting from a caller-cached entry hint.  The
 * hint is re-validated (retirement + key match) under the epoch; a stale
 * hint degrades to a full bucket walk, never to a wrong serve.  Returns the
 * blob bytes with the pin held, or NULL with no pin held. */
static const void *bdr_serve_hit(cwist_bdr_t *bdr, uint64_t req_h, bdr_entry_t *hint,
                                 size_t *out_len, bdr_blob_t **out_pin) {
    ttak_epoch_enter();
    bdr_entry_t *entry = hint;
    if (entry &&
        (atomic_load_explicit(&entry->retired, memory_order_acquire) ||
         entry->request_hash != req_h)) {
        entry = NULL;
    }
    if (!entry) entry = bdr_find(bdr, req_h);
    if (!entry) {
        ttak_epoch_exit();
        return NULL;
    }
    atomic_fetch_add_explicit(&entry->hits, 1, memory_order_relaxed);

    /* Hit-time revalidation: a true report swaps in the callback's buffer
     * with one pointer exchange; the predecessor retires across an epoch. */
    cwist_bdr_revalidate_fn hook = entry->revalidate;
    if (hook) {
        void *fresh = NULL;
        size_t fresh_len = 0;
        void (*fresh_free)(void *) = NULL;
        if (hook(entry->revalidate_arg, &fresh, &fresh_len, &fresh_free) && fresh &&
            fresh_len > 0) {
            bdr_blob_t *nb = bdr_blob_wrap(fresh, fresh_len, fresh_free);
            if (nb) {
                bdr_entry_publish(bdr, entry, nb);
            } else if (fresh_free) {
                fresh_free(fresh);
            }
        }
    }

    bdr_blob_t *blob = atomic_load_explicit(&entry->blob, memory_order_acquire);
    if (!blob || !atomic_load_explicit(&entry->is_stable, memory_order_acquire)) {
        ttak_epoch_exit();
        return NULL;
    }
    if (out_len) *out_len = blob->len;
    *out_pin = blob;
    return blob->data;
}

const void *cwist_bdr_get_pinned(cwist_bdr_t *bdr, const char *method, const char *path,
                                 size_t *out_len, bdr_blob_t **out_pin) {
    if (!bdr || !method || !path || !out_pin) return NULL;
    if (strcmp(method, "GET") != 0) return NULL;
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) return NULL;

    return bdr_serve_hit(bdr, bdr_hash(method, path), NULL, out_len, out_pin);
}

const void *cwist_bdr_get_pinned_cursor(cwist_bdr_t *bdr, const char *method, const char *path,
                                        size_t path_len, size_t *out_len, bdr_blob_t **out_pin,
                                        cwist_bdr_cursor_t *cursor) {
    if (!bdr || !method || !path || !out_pin || !cursor) return NULL;
    if (strcmp(method, "GET") != 0) return NULL;
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) return NULL;

    uint64_t req_h;
    bdr_entry_t *hint = NULL;
    if (cursor->entry && cursor->path_len == path_len &&
        path_len < CWIST_BDR_CURSOR_PATH_MAX && cursor->req_hash != 0 &&
        memcmp(cursor->path, path, path_len) == 0) {
        hint = cursor->entry;
        req_h = cursor->req_hash;
    } else {
        req_h = bdr_hash_n(method, path, path_len);
    }

    const void *data = bdr_serve_hit(bdr, req_h, hint, out_len, out_pin);
    if (data && path_len < CWIST_BDR_CURSOR_PATH_MAX) {
        bdr_entry_t *served = hint;
        if (!served || atomic_load_explicit(&served->retired, memory_order_relaxed) ||
            served->request_hash != req_h) {
            /* The hint was stale (or absent): recover the served entry from
             * the bucket chain so the cursor points at the live node. */
            served = bdr_find(bdr, req_h);
        }
        if (served) {
            cursor->entry = served;
            cursor->req_hash = req_h;
            cursor->path_len = path_len;
            memcpy(cursor->path, path, path_len);
        } else {
            cursor->entry = NULL;
        }
    } else if (!data) {
        cursor->entry = NULL;
    }
    return data;
}

void cwist_bdr_unpin(bdr_blob_t *pin) {
    (void)pin; /* The pin is the epoch itself. */
    ttak_epoch_exit();
}

const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    if (!bdr || !method || !path || bdr->is_disk_mode) return NULL;
    uint64_t req_h = bdr_hash(method, path);
    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *curr = __atomic_load_n(&bdr->buckets[idx], __ATOMIC_ACQUIRE);
    while (curr) {
        if (curr->request_hash == req_h) {
            if (curr->is_stable && curr->response_blob) {
                if (out_len) *out_len = curr->len;
                return curr->response_blob;
            }
            return NULL;
        }
        curr = curr->next;
    }
    return NULL;
}

void *cwist_bdr_copy_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    if (!bdr) return NULL;
    pthread_mutex_lock(&bdr->lock);
    size_t len = 0;
    const void *blob = bdr_get_locked(bdr, method, path, &len);
    void *copy = NULL;
    if (blob && len > 0) {
        copy = cwist_alloc(len);
        if (copy) {
            memcpy(copy, blob, len);
            if (out_len) *out_len = len;
        }
    }
    pthread_mutex_unlock(&bdr->lock);
    return copy;
}



/**
 * @brief Feed a candidate GET response into the cache and promote stable blobs.
 * @param bdr Cache instance to update.
 * @param method HTTP method string.
 * @param path Canonical request path.
 * @param data Serialized response bytes.
 * @param len Number of bytes in @p data.
 */
void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {

    if (!bdr || !method || !path || !data || len == 0) return;

    if (strcmp(method, "GET") != 0) return;

    pthread_mutex_lock(&bdr->lock);



    // Check RAM health before adding

    bdr_check_ram(bdr);



    uint64_t req_h = bdr_hash(method, path);

    uint64_t res_h = bdr_hash_data(data, len);



    if (bdr->is_disk_mode) {

         // In disk mode, we assume stability check is too expensive or we just dump.

         // Actually, to respect "Stability Check" even on disk, we need read-modify-write.

         // But disk is fallback. Let's just update.

         // Or simpler: Disk mode = Emergency. Just save it.

         sqlite3_stmt *stmt;

         sqlite3_prepare_v2(bdr->disk_db, "INSERT OR REPLACE INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);

         sqlite3_bind_int64(stmt, 1, req_h);

         sqlite3_bind_blob(stmt, 2, data, len, SQLITE_STATIC);

         sqlite3_step(stmt);

         sqlite3_finalize(stmt);

         bdr_guardrails(bdr);

         pthread_mutex_unlock(&bdr->lock);
         return;

    }



    size_t idx = req_h % bdr->bucket_count;

    

    // Check exist

    bdr_entry_t *curr = bdr->buckets[idx];

    while (curr) {

        if (curr->request_hash == req_h) {

            // Entry exists. Check stability.

            if (curr->is_stable) {

                // Already stable. 

                // Optional: Re-verify occasionally? For now, assume "Big Dumb" means permanent.

                // If we want to detect changes:

                if (curr->response_hash != res_h) {

                    // Changed! Invalidated.

                    // Downgrade to unstable? Or update immediately?

                    // "Only cache if totally matching".

                    // If it changed, it's not dumb-cacheable.

                    // Evict it.

                    curr->is_stable = false;

                    free(curr->response_blob);

                    curr->response_blob = NULL;

                    curr->response_hash = res_h; // New candidate

                }

            } else {

                // Was unstable/candidate. Check if matches candidate.

                if (curr->response_hash == res_h) {

                    // Match! Stabilize.

                    curr->response_blob = malloc(len);

                    if (curr->response_blob) {

                        memcpy(curr->response_blob, data, len);

                        curr->len = len;

                        curr->is_stable = true;

                        // printf("[BDR] Stabilized: %s\n", path);

                    }

                } else {

                    // Mismatch. Keep unstable, update candidate.

                    curr->response_hash = res_h;

                }

            }

            pthread_mutex_unlock(&bdr->lock);
            return;

        }

        curr = curr->next;

    }

    

    // New Entry (Candidate)

    bdr_entry_t *entry = malloc(sizeof(bdr_entry_t));

    if (!entry) {
        pthread_mutex_unlock(&bdr->lock);
        return;
    }



    entry->request_hash = req_h;

    entry->response_hash = res_h;

    entry->is_stable = false; // Start as candidate

    entry->response_blob = NULL;

    entry->len = 0;

    entry->hits = 0;

    entry->created_at = time(NULL);

    

    entry->next = __atomic_load_n(&bdr->buckets[idx], __ATOMIC_RELAXED);
    __atomic_store_n(&bdr->buckets[idx], entry, __ATOMIC_RELEASE);

    bdr_guardrails(bdr);

    pthread_mutex_unlock(&bdr->lock);
}

void cwist_bdr_put_fixed(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {
    if (!bdr || !method || !path || !data || len == 0) return;
    if (strcmp(method, "GET") != 0) return;

    pthread_mutex_lock(&bdr->lock);
    bdr_check_ram(bdr);

    uint64_t req_h = bdr_hash(method, path);
    uint64_t res_h = bdr_hash_data(data, len);

    if (bdr->is_disk_mode) {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(bdr->disk_db, "INSERT OR REPLACE INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, req_h);
        sqlite3_bind_blob(stmt, 2, data, len, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        bdr_guardrails(bdr);
        pthread_mutex_unlock(&bdr->lock);
        return;
    }

    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *curr = bdr->buckets[idx];
    while (curr) {
        if (curr->request_hash == req_h) {
            void *blob = cwist_alloc(len);
            if (blob) {
                memcpy(blob, data, len);
                bdr_release_blob(bdr, curr);
                curr->response_blob = blob;
                curr->len = len;
                curr->response_hash = res_h;
                curr->is_stable = true;
                curr->hits = 0;
                curr->created_at = time(NULL);
                bdr->current_bytes += len;
                bdr_guardrails(bdr);
            }
            pthread_mutex_unlock(&bdr->lock);
            return;
        }
        curr = curr->next;
    }

    bdr_entry_t *entry = cwist_alloc(sizeof(bdr_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&bdr->lock);
        return;
    }

    void *blob = cwist_alloc(len);
    if (!blob) {
        cwist_free(entry);
        pthread_mutex_unlock(&bdr->lock);
        return;
    }
    memcpy(blob, data, len);

    entry->request_hash = req_h;
    entry->response_hash = res_h;
    entry->is_stable = true;
    entry->response_blob = blob;
    entry->len = len;
    entry->hits = 0;
    entry->created_at = time(NULL);
    bdr->current_bytes += len;

    entry->next = __atomic_load_n(&bdr->buckets[idx], __ATOMIC_RELAXED);
    __atomic_store_n(&bdr->buckets[idx], entry, __ATOMIC_RELEASE);

    bdr_guardrails(bdr);
    pthread_mutex_unlock(&bdr->lock);
}

void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits) {

    if (!bdr) return;

    pthread_mutex_lock(&bdr->lock);

    if (max_bytes > 0) {

        bdr->max_bytes = max_bytes;

    }

    if (max_entry_age_sec > 0) {

        bdr->max_entry_age_sec = max_entry_age_sec;

    }

    if (revalidate_hits > 0) {

        bdr->revalidate_hits = revalidate_hits;

    }

    bdr_guardrails(bdr);

    pthread_mutex_unlock(&bdr->lock);

}
