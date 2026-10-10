#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <ttak/mem/epoch.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/**
 * @file big_dumb_reply.c
 * @brief Opportunistic response cache that stores stable GET replies in memory or on disk.
 *
 * Concurrency model: bucket chains are published with a CAS push and entry
 * nodes are never freed until destroy (retired entries are tombstoned), so
 * readers walk without locks.  The response blob lives behind an atomic
 * pointer per entry; writers swap it with one exchange and retire the
 * predecessor through libttak EBR, which guarantees a swapped-out blob
 * outlives every reader that entered its epoch before the swap.  Only the
 * janitor (TTL/byte-trim/disk spill, every 64 learns) and the disk-fallback
 * path take the context mutex.
 */

#define BDR_BUCKETS 1024
#define BDR_GC_SWEEP 8
#define BDR_JANITOR_PERIOD 64
#define BDR_DEFAULT_MAX_BYTES CWIST_MIB(32)
#define BDR_DEFAULT_ENTRY_TTL 300
#define BDR_DEFAULT_REVALIDATE_HITS 100000

/**
 * @brief Static SipHash key used to bucket request and response fingerprints.
 */
static const uint8_t BDR_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/* --- Blob lifetime -------------------------------------------------------- */

/**
 * @brief Epoch retire callback: release a blob and any buffer it owns.
 *
 * Reads the owned buffer and its free function before freeing, because for
 * inline (learned) blobs the buffer aliases the blob header itself.
 *
 * @param ptr Blob to free; NULL is tolerated.
 */
static void bdr_blob_free_cb(void *ptr) {
    bdr_blob_t *blob = (bdr_blob_t *)ptr;
    if (!blob) return;
    /* Read the fields before releasing: for inline blobs mem == blob, so the
     * free below clobbers the very bytes the branch would inspect (glibc
     * writes its tcache links into freed chunks). */
    void *mem = blob->mem;
    void (*free_fn)(void *) = blob->free_fn;
    free_fn(mem);
    if (mem != ptr) cwist_free(blob);
}

/**
 * @brief Retire a swapped-out blob; the epoch guarantees no reader still serves it.
 * @param blob Blob to retire; NULL is a no-op.
 */
static void bdr_blob_retire(bdr_blob_t *blob) {
    if (blob) ttak_epoch_retire(blob, bdr_blob_free_cb);
}

/**
 * @brief Build a learned blob: one allocation carries header and bytes (single copy from
 * the caller-owned serialization, whose lifetime the cache cannot borrow).
 * @return New blob on success, NULL on allocation failure.
 */
static bdr_blob_t *bdr_blob_learn(const void *data, size_t len) {
    bdr_blob_t *blob = cwist_alloc(sizeof(bdr_blob_t) + len);
    if (!blob) return NULL;
    blob->mem = blob;
    blob->free_fn = cwist_free;
    blob->len = len;
    blob->data = (const unsigned char *)(blob + 1);
    memcpy((void *)blob->data, data, len);
    return blob;
}

/**
 * @brief Build a callback-provided blob: header only, the buffer pointer is swung as-is.
 * @param mem Buffer owned by the caller/callback; freed via @p free_fn.
 * @param free_fn Deallocator for @p mem; defaults to cwist_free when NULL.
 * @return New blob on success, NULL on allocation failure.
 */
static bdr_blob_t *bdr_blob_wrap(void *mem, size_t len, void (*free_fn)(void *)) {
    bdr_blob_t *blob = cwist_alloc(sizeof(bdr_blob_t));
    if (!blob) return NULL;
    blob->mem = mem;
    blob->free_fn = free_fn ? free_fn : cwist_free;
    blob->len = len;
    blob->data = (const unsigned char *)mem;
    return blob;
}

/**
 * @brief Add @p len bytes to the cache's current byte counter (relaxed atomic).
 * @param bdr Cache instance.
 * @param len Number of bytes to add.
 */
static void bdr_bytes_add(cwist_bdr_t *bdr, size_t len) {
    atomic_fetch_add_explicit(&bdr->current_bytes, len, memory_order_relaxed);
}

/**
 * @brief Subtract @p len bytes from the cache's current byte counter, clamping at zero.
 * @param bdr Cache instance.
 * @param len Number of bytes to subtract.
 */
static void bdr_bytes_sub(cwist_bdr_t *bdr, size_t len) {
    size_t cur = atomic_load_explicit(&bdr->current_bytes, memory_order_relaxed);
    while (cur > 0) {
        size_t next = cur >= len ? cur - len : 0;
        if (atomic_compare_exchange_weak_explicit(&bdr->current_bytes, &cur, next,
                                                  memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

/**
 * @brief Swap a new blob into an entry and retire whatever was published before.
 *
 * Keeps the byte counter in sync and retires the predecessor through EBR so
 * readers inside its epoch keep a valid blob.
 *
 * @param bdr Cache instance (byte accounting).
 * @param entry Entry whose blob pointer is exchanged.
 * @param blob New blob, or NULL to clear the entry.
 */
static void bdr_entry_publish(cwist_bdr_t *bdr, bdr_entry_t *entry, bdr_blob_t *blob) {
    bdr_blob_t *old = atomic_exchange_explicit(&entry->blob, blob, memory_order_acq_rel);
    if (blob) bdr_bytes_add(bdr, blob->len);
    if (old) {
        bdr_bytes_sub(bdr, old->len);
        bdr_blob_retire(old);
    }
}

/* --- Lookup helpers ------------------------------------------------------- */

/**
 * @brief Hash a request key (method first byte mixed into the path SipHash).
 * @param method HTTP method string; only the first byte contributes.
 * @param path NUL-terminated request path.
 * @return 64-bit bucket hash.
 */
static uint64_t bdr_hash(const char *method, const char *path) {
    uint64_t h = siphash24((const void *)path, strlen(path), BDR_KEY);
    h ^= (uint64_t)(method[0]);
    return h;
}

/**
 * @brief Hash a request key from a non-NUL-terminated path.
 * @param method HTTP method string; only the first byte contributes.
 * @param path Request path bytes (not required to be NUL-terminated).
 * @param path_len Length of @p path in bytes.
 * @return 64-bit bucket hash.
 */
static uint64_t bdr_hash_n(const char *method, const char *path, size_t path_len) {
    uint64_t h = siphash24((const void *)path, path_len, BDR_KEY);
    h ^= (uint64_t)(method[0]);
    return h;
}

/**
 * @brief Hash response bytes (fingerprint used to detect content changes).
 * @param data Response bytes.
 * @param len Length of @p data in bytes.
 * @return 64-bit response hash.
 */
static uint64_t bdr_hash_data(const void *data, size_t len) {
    return siphash24(data, len, BDR_KEY);
}

/**
 * @brief Decide whether an entry should be retired by the janitor.
 *
 * An entry decays when its age exceeds @c max_entry_age_sec, or when it is
 * stable and has accumulated at least @c revalidate_hits hits.
 *
 * @param bdr Cache instance (limits; may be NULL).
 * @param entry Entry to inspect (may be NULL).
 * @param now Current time.
 * @retval true The entry should be retired.
 * @retval false The entry stays live, or arguments are NULL.
 */
static bool bdr_entry_should_decay(const cwist_bdr_t *bdr, const bdr_entry_t *entry, time_t now) {
    if (!bdr || !entry) return false;
    int64_t created = atomic_load_explicit(&entry->created_at, memory_order_relaxed);
    if (bdr->max_entry_age_sec > 0 && created > 0) {
        if (now - (time_t)created > bdr->max_entry_age_sec) return true;
    }
    if (atomic_load_explicit(&entry->is_stable, memory_order_relaxed) && bdr->revalidate_hits > 0 &&
        atomic_load_explicit(&entry->hits, memory_order_relaxed) >= bdr->revalidate_hits) {
        return true;
    }
    return false;
}

/**
 * @brief Walk a bucket chain for a precomputed request hash.
 *
 * Entries are tombstoned rather than freed, so the walk is safe without
 * locks as long as loads are atomic.  Retired entries are skipped.
 *
 * @param bdr Cache instance.
 * @param req_h Precomputed request hash.
 * @return Matching live entry, or NULL if none.
 */
static bdr_entry_t *bdr_find(cwist_bdr_t *bdr, uint64_t req_h) {
    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
    while (curr) {
        if (curr->request_hash == req_h &&
            !atomic_load_explicit(&curr->retired, memory_order_acquire)) {
            return curr;
        }
        curr = atomic_load_explicit(&curr->next, memory_order_acquire);
    }
    return NULL;
}

/**
 * @brief Look up an entry by request hash, inserting a fresh candidate at the
 * bucket head if absent (lock-free CAS push).
 *
 * On a CAS race with a concurrent insert of the same key, the losing entry is
 * freed and the winner is returned.
 *
 * @param bdr Cache instance.
 * @param req_h Precomputed request hash.
 * @return The entry that ended up reachable (ours, or the winner of a CAS race),
 *         or NULL on allocation failure.
 */
static bdr_entry_t *bdr_find_or_insert(cwist_bdr_t *bdr, uint64_t req_h) {
    bdr_entry_t *found = bdr_find(bdr, req_h);
    if (found) return found;

    bdr_entry_t *entry = cwist_alloc(sizeof(bdr_entry_t));
    if (!entry) return NULL;
    entry->request_hash = req_h;
    atomic_init(&entry->response_hash, 0);
    atomic_init(&entry->is_stable, false);
    atomic_init(&entry->retired, false);
    atomic_init(&entry->blob, NULL);
    atomic_init(&entry->hits, 0);
    atomic_init(&entry->created_at, (int64_t)time(NULL));
    entry->revalidate = NULL;
    entry->revalidate_arg = NULL;
    entry->retire_next = NULL;

    size_t idx = req_h % bdr->bucket_count;
    bdr_entry_t *head = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
    do {
        /* A concurrent insert of the same key wins; ours becomes garbage. */
        bdr_entry_t *race = bdr_find(bdr, req_h);
        if (race) {
            cwist_free(entry);
            return race;
        }
        atomic_store_explicit(&entry->next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&bdr->buckets[idx], &head, entry,
                                                    memory_order_release, memory_order_acquire));
    return entry;
}

/* --- Janitor (mutex-serialized, runs every BDR_JANITOR_PERIOD learns) ----- */

/**
 * @brief Unlink an entry from its bucket chain and push it onto the retired list.
 *
 * Tombstones the entry (readers may still hold it), clears its blob, and moves
 * it to @c retired_entries for deferred reclamation at destroy.  Must be called
 * with the janitor lock held or during teardown.
 *
 * @param bdr Cache instance.
 * @param idx Bucket index of the entry.
 * @param prev Predecessor in the chain, or NULL if @p entry is the bucket head.
 * @param entry Entry to retire.
 */
static void bdr_retire_entry(cwist_bdr_t *bdr, size_t idx, bdr_entry_t *prev, bdr_entry_t *entry) {
    atomic_store_explicit(&entry->retired, true, memory_order_release);
    if (prev) {
        atomic_store_explicit(&prev->next, atomic_load_explicit(&entry->next, memory_order_acquire),
                              memory_order_release);
    } else {
        atomic_store_explicit(&bdr->buckets[idx],
                              atomic_load_explicit(&entry->next, memory_order_acquire),
                              memory_order_release);
    }
    bdr_entry_publish(bdr, entry, NULL);
    entry->retire_next = bdr->retired_entries;
    bdr->retired_entries = entry;
}

/**
 * @brief Incremental GC sweep: visit @p steps buckets and retire decayed entries.
 *
 * Advances @c gc_cursor so successive calls cover the table over time.
 *
 * @param bdr Cache instance.
 * @param steps Number of buckets to visit; 0 is a no-op.
 */
static void bdr_sweep(cwist_bdr_t *bdr, size_t steps) {
    if (!bdr || bdr->bucket_count == 0 || steps == 0) return;
    time_t now = time(NULL);
    for (size_t i = 0; i < steps; ++i) {
        size_t idx = bdr->gc_cursor % bdr->bucket_count;
        bdr->gc_cursor = (bdr->gc_cursor + 1) % bdr->bucket_count;
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[idx], memory_order_acquire);
        while (curr) {
            bdr_entry_t *next = atomic_load_explicit(&curr->next, memory_order_acquire);
            if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                bdr_entry_should_decay(bdr, curr, now)) {
                bdr_retire_entry(bdr, idx, prev, curr);
            } else {
                prev = curr;
            }
            curr = next;
        }
    }
}

/**
 * @brief Find the oldest live, blob-backed entry and retire it.
 *
 * Scans the whole table; used to enforce the @c max_bytes byte budget.
 *
 * @param bdr Cache instance.
 * @param now Current time (baseline for the oldest comparison).
 * @retval true An entry was retired.
 * @retval false No eligible entry exists.
 */
static bool bdr_trim_oldest(cwist_bdr_t *bdr, time_t now) {
    size_t victim_idx = SIZE_MAX;
    bdr_entry_t *victim = NULL;
    bdr_entry_t *victim_prev = NULL;
    int64_t oldest = (int64_t)now;

    for (size_t i = 0; i < bdr->bucket_count; ++i) {
        bdr_entry_t *prev = NULL;
        bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[i], memory_order_acquire);
        while (curr) {
            if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                atomic_load_explicit(&curr->blob, memory_order_relaxed)) {
                int64_t created = atomic_load_explicit(&curr->created_at, memory_order_relaxed);
                if (!victim || created < oldest) {
                    victim = curr;
                    victim_prev = prev;
                    victim_idx = i;
                    oldest = created;
                }
            }
            prev = curr;
            curr = atomic_load_explicit(&curr->next, memory_order_acquire);
        }
    }

    if (!victim) return false;
    bdr_retire_entry(bdr, victim_idx, victim_prev, victim);
    return true;
}

/**
 * @brief Spill the in-RAM cache to a SQLite fallback database when RAM is critical.
 *
 * When free RAM drops below the 64 MiB threshold, copies every stable entry's
 * blob into "cwist_bdr_fallback.db", retires all live entries, and flips the
 * cache into disk mode so subsequent reads miss and writes go to SQLite.
 * No-op in the WASM build and once disk mode is already active.  Must be
 * called with the janitor lock held.
 *
 * @param bdr Cache instance.
 */
static void bdr_check_ram(cwist_bdr_t *bdr) {
#ifdef __EMSCRIPTEN__
    /* No SQLite disk fallback in the WASM core; the in-RAM cache stands alone. */
    (void)bdr;
    return;
#else
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_relaxed)) return;

    if (cwist_is_ram_critical(CWIST_MIB(64))) {
        printf("[BDR] Low RAM. Switching to Disk Cache.\n");
        if (sqlite3_open("cwist_bdr_fallback.db", &bdr->disk_db) == SQLITE_OK) {
            char *err = NULL;
            sqlite3_exec(bdr->disk_db,
                         "CREATE TABLE IF NOT EXISTS bdr (hash INTEGER PRIMARY KEY, blob BLOB);",
                         NULL, NULL, &err);
            if (err) sqlite3_free(err);

            sqlite3_exec(bdr->disk_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
            for (size_t i = 0; i < bdr->bucket_count; i++) {
                bdr_entry_t *curr = atomic_load_explicit(&bdr->buckets[i], memory_order_acquire);
                while (curr) {
                    bdr_entry_t *next = atomic_load_explicit(&curr->next, memory_order_acquire);
                    if (!atomic_load_explicit(&curr->retired, memory_order_relaxed) &&
                        atomic_load_explicit(&curr->is_stable, memory_order_relaxed)) {
                        bdr_blob_t *blob = atomic_load_explicit(&curr->blob, memory_order_acquire);
                        if (blob) {
                            sqlite3_stmt *stmt;
                            sqlite3_prepare_v2(bdr->disk_db,
                                               "INSERT INTO bdr (hash, blob) VALUES (?, ?);", -1,
                                               &stmt, NULL);
                            sqlite3_bind_int64(stmt, 1, curr->request_hash);
                            sqlite3_bind_blob(stmt, 2, blob->data, (int)blob->len, SQLITE_STATIC);
                            sqlite3_step(stmt);
                            sqlite3_finalize(stmt);
                        }
                    }
                    if (!atomic_load_explicit(&curr->retired, memory_order_relaxed)) {
                        atomic_store_explicit(&curr->retired, true, memory_order_release);
                        bdr_entry_publish(bdr, curr, NULL);
                        curr->retire_next = bdr->retired_entries;
                        bdr->retired_entries = curr;
                    }
                    curr = next;
                }
                atomic_store_explicit(&bdr->buckets[i], NULL, memory_order_release);
            }
            sqlite3_exec(bdr->disk_db, "COMMIT;", NULL, NULL, NULL);
            atomic_store_explicit(&bdr->is_disk_mode, true, memory_order_release);
        }
    }
#endif
}

/**
 * @brief Periodic janitor hook, invoked from the lock-free learn path.
 *
 * Runs every @c BDR_JANITOR_PERIOD puts: checks for a RAM-critical disk spill,
 * sweeps a few buckets for decayed entries, and trims the oldest entries while
 * the byte budget is exceeded.  Serialized with a trylock so a contended tick
 * is skipped rather than blocking the learn path.
 *
 * @param bdr Cache instance.
 */
static void bdr_janitor_tick(cwist_bdr_t *bdr) {
    uint64_t n = atomic_fetch_add_explicit(&bdr->put_count, 1, memory_order_relaxed) + 1;
    if (n % BDR_JANITOR_PERIOD != 0) return;
    if (pthread_mutex_trylock(&bdr->lock) != 0) return; /* another tick holds it */
    bdr_check_ram(bdr);
    bdr_sweep(bdr, BDR_GC_SWEEP);
    if (bdr->max_bytes > 0) {
        time_t now = time(NULL);
        while (atomic_load_explicit(&bdr->current_bytes, memory_order_relaxed) > bdr->max_bytes) {
            if (!bdr_trim_oldest(bdr, now)) break;
        }
    }
    pthread_mutex_unlock(&bdr->lock);
}

/* --- Lifecycle ------------------------------------------------------------ */

/**
 * @brief Create a reply cache instance with default limits.
 *
 * Initializes the mutex, the bucket table, and the default tuning values
 * (32 MiB byte budget, 300 s entry TTL, 100000 revalidate hits).
 *
 * @return New cache instance on success, NULL on allocation or mutex failure.
 */
cwist_bdr_t *cwist_bdr_create(void) {
    cwist_bdr_t *bdr = cwist_alloc(sizeof(cwist_bdr_t));
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
    atomic_init(&bdr->current_bytes, 0);
    bdr->max_bytes = BDR_DEFAULT_MAX_BYTES;
    bdr->max_entry_age_sec = BDR_DEFAULT_ENTRY_TTL;
    bdr->revalidate_hits = BDR_DEFAULT_REVALIDATE_HITS;
    bdr->gc_cursor = 0;
    atomic_init(&bdr->put_count, 0);
    bdr->retired_entries = NULL;
    bdr->disk_db = NULL;
    atomic_init(&bdr->is_disk_mode, false);
    return bdr;
}

/**
 * @brief Destroy a cache instance and release all its resources.
 *
 * Frees every live and retired entry and its blob, the bucket table, and the
 * disk fallback database (including the "cwist_bdr_fallback.db" file).  Safe
 * to call with NULL.
 *
 * @param bdr Cache instance to destroy, or NULL.
 */
void cwist_bdr_destroy(cwist_bdr_t *bdr) {
    if (!bdr) return;
    pthread_mutex_lock(&bdr->lock);
    for (size_t i = 0; i < bdr->bucket_count; i++) {
        bdr_entry_t *curr = bdr->buckets[i];
        while (curr) {
            bdr_entry_t *next = curr->next;
            if (curr == NULL) break;
            if (curr->blob != NULL)
                bdr_blob_free_cb(atomic_load_explicit(&curr->blob, memory_order_relaxed));
            cwist_free(curr);
            curr = next;
        }
    }
    while (bdr->retired_entries) {
        bdr_entry_t *next = bdr->retired_entries->retire_next;
        bdr_blob_free_cb(atomic_load_explicit(&bdr->retired_entries->blob, memory_order_relaxed));
        cwist_free(bdr->retired_entries);
        bdr->retired_entries = next;
    }
    cwist_free(bdr->buckets);
#ifndef __EMSCRIPTEN__
    if (bdr->disk_db) {
        sqlite3_close(bdr->disk_db);
        remove("cwist_bdr_fallback.db");
    }
#endif
    pthread_mutex_unlock(&bdr->lock);
    pthread_mutex_destroy(&bdr->lock);
    cwist_free(bdr);
}

/* --- Read path ------------------------------------------------------------ */

/**
 * @brief Shared read core: epoch-protected hit validation for a precomputed
 * request hash, optionally starting from a caller-cached entry hint.
 *
 * The hint is re-validated (retirement + key match) under the epoch; a stale
 * hint degrades to a full bucket walk, never to a wrong serve.
 *
 * @param bdr Cache instance.
 * @param req_h Precomputed request hash.
 * @param hint Candidate entry from a cursor, or NULL for a full lookup.
 * @param out_len Receives the blob length on a hit (may be NULL).
 * @param out_pin Receives the pinned blob; released with cwist_bdr_unpin().
 * @return Blob bytes with the pin held, or NULL with no pin held.
 */
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

/**
 * @brief Look up a cached GET reply, returning epoch-pinned bytes.
 *
 * Only GET requests are served, and only while the cache is in RAM mode.  The
 * caller must release the pin with cwist_bdr_unpin() once done reading.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param out_len Receives the reply length on a hit (may be NULL).
 * @param out_pin Receives the pin to pass to cwist_bdr_unpin().
 * @return Reply bytes pinned for the caller, or NULL on miss/invalid input.
 */
const void *cwist_bdr_get_pinned(cwist_bdr_t *bdr, const char *method, const char *path,
                                 size_t *out_len, bdr_blob_t **out_pin) {
    if (!bdr || !method || !path || !out_pin) return NULL;
    if (strcmp(method, "GET") != 0) return NULL;
    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) return NULL;

    return bdr_serve_hit(bdr, bdr_hash(method, path), NULL, out_len, out_pin);
}

/**
 * @brief Cursor-accelerated variant of cwist_bdr_get_pinned() for non-NUL-terminated paths.
 *
 * When the cursor's cached entry still matches the request it is used as a
 * lookup hint; a stale hint degrades to a full bucket walk.  On a hit the
 * cursor is refreshed to point at the served entry, so repeated reads of the
 * same path skip the chain walk.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path Request path bytes (not required to be NUL-terminated).
 * @param path_len Length of @p path in bytes.
 * @param out_len Receives the reply length on a hit (may be NULL).
 * @param out_pin Receives the pin to pass to cwist_bdr_unpin().
 * @param cursor Reusable lookup cursor, updated on every call.
 * @return Reply bytes pinned for the caller, or NULL on miss/invalid input.
 */
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

/**
 * @brief Release a pin obtained from a cwist_bdr_get_pinned*() call.
 *
 * The pin is the EBR epoch itself, so this simply leaves the epoch.
 *
 * @param pin Pin to release (ignored beyond symmetry with the get calls).
 */
void cwist_bdr_unpin(bdr_blob_t *pin) {
    (void)pin; /* The pin is the epoch itself. */
    ttak_epoch_exit();
}

/**
 * @brief Look up a cached GET reply and return a borrowed pointer to its bytes.
 *
 * Convenience wrapper over cwist_bdr_get_pinned() that unpins immediately, so
 * the returned pointer is only valid until the next janitor activity that
 * could retire the entry; prefer the pinned API for anything but a memcpy.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param out_len Receives the reply length on a hit (may be NULL).
 * @return Reply bytes (unpinned), or NULL on miss/invalid input.
 */
const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    bdr_blob_t *pin = NULL;
    const void *data = cwist_bdr_get_pinned(bdr, method, path, out_len, &pin);
    if (pin) cwist_bdr_unpin(pin);
    return data;
}

/**
 * @brief Look up a cached GET reply and return an owned copy of its bytes.
 *
 * Unlike cwist_bdr_get(), the returned buffer is allocated with cwist_alloc()
 * and survives cache eviction; the caller frees it with cwist_free().
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param out_len Receives the copy length on a hit (may be NULL).
 * @return Newly allocated copy of the reply, or NULL on miss/invalid input/allocation failure.
 */
void *cwist_bdr_copy_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {
    bdr_blob_t *pin = NULL;
    size_t len = 0;
    const void *data = cwist_bdr_get_pinned(bdr, method, path, &len, &pin);
    void *copy = NULL;
    if (data && len > 0) {
        copy = cwist_alloc(len);
        if (copy) {
            memcpy(copy, data, len);
            if (out_len) *out_len = len;
        }
    }
    if (pin) cwist_bdr_unpin(pin);
    return copy;
}

/* --- Learn path (lock-free) ------------------------------------------------ */

/**
 * @brief Write a reply into the SQLite fallback database (disk mode).
 *
 * Must be called only while the cache is in disk mode; takes the context
 * mutex for the duration of the statement.
 *
 * @param bdr Cache instance (disk_db must be open).
 * @param req_h Precomputed request hash (primary key).
 * @param data Response bytes.
 * @param len Length of @p data in bytes.
 */
static void cwist_bdr_put_disk(cwist_bdr_t *bdr, uint64_t req_h, const void *data, size_t len) {
    pthread_mutex_lock(&bdr->lock);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(bdr->disk_db, "INSERT OR REPLACE INTO bdr (hash, blob) VALUES (?, ?);", -1,
                       &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)req_h);
    sqlite3_bind_blob(stmt, 2, data, (int)len, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&bdr->lock);
}

/**
 * @brief Learn a reply observation into the cache (lock-free fast path).
 *
 * Only GET replies are tracked.  An entry stabilizes once the same response
 * bytes are seen twice in a row; a content change under a stable entry demotes
 * it back to candidate.  In disk mode the observation is written to SQLite
 * instead.  Triggers the janitor tick periodically.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param data Observed response bytes.
 * @param len Length of @p data in bytes.
 */
void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data,
                   size_t len) {
    if (!bdr || !method || !path || !data || len == 0) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);
    uint64_t res_h = bdr_hash_data(data, len);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    if (atomic_load_explicit(&entry->is_stable, memory_order_acquire)) {
        /* Content changed under a stable entry: demote to candidate. */
        if (atomic_load_explicit(&entry->response_hash, memory_order_relaxed) != res_h) {
            bdr_entry_publish(bdr, entry, NULL);
            atomic_store_explicit(&entry->response_hash, res_h, memory_order_release);
            atomic_store_explicit(&entry->is_stable, false, memory_order_release);
            atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
            atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);
        }
    } else {
        uint64_t expect = res_h;
        if (atomic_compare_exchange_strong_explicit(&entry->response_hash, &expect, res_h,
                                                    memory_order_acq_rel, memory_order_relaxed)) {
            /* Candidate reproduced the same bytes: stabilize.  A concurrent
             * stabilizer publishes an identical blob; the loser's duplicate
             * comes back from the exchange and is retired unused. */
            bdr_blob_t *blob = bdr_blob_learn(data, len);
            if (blob) {
                bdr_entry_publish(bdr, entry, blob);
                atomic_store_explicit(&entry->is_stable, true, memory_order_release);
                atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
                atomic_store_explicit(&entry->created_at, (int64_t)time(NULL),
                                      memory_order_relaxed);
            }
        } else {
            /* Different bytes: the entry now tracks the new candidate hash. */
            atomic_store_explicit(&entry->response_hash, res_h, memory_order_release);
        }
    }

    bdr_janitor_tick(bdr);
}

/**
 * @brief Publish a reply as immediately stable, bypassing the learning phase.
 *
 * For responses that are known to be constant; stores the bytes and marks the
 * entry stable in one shot.  In disk mode the observation is written to
 * SQLite instead.  Triggers the janitor tick periodically.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param data Response bytes to cache.
 * @param len Length of @p data in bytes.
 */
void cwist_bdr_put_fixed(cwist_bdr_t *bdr, const char *method, const char *path, const void *data,
                         size_t len) {
    if (!bdr || !method || !path || !data || len == 0) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    bdr_blob_t *blob = bdr_blob_learn(data, len);
    if (!blob) return;
    atomic_store_explicit(&entry->response_hash, bdr_hash_data(data, len), memory_order_release);
    bdr_entry_publish(bdr, entry, blob);
    atomic_store_explicit(&entry->is_stable, true, memory_order_release);
    atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);

    bdr_janitor_tick(bdr);
}

/**
 * @brief Publish a stable reply together with a hit-time revalidation hook.
 *
 * On every subsequent hit the hook is invoked and, if it reports fresh
 * bytes, the entry's blob is swapped to the callback-provided buffer through
 * one pointer exchange.  The hook is published before the blob so a reader
 * never observes a revalidatable blob without its hook.  In disk mode the
 * observation is written to SQLite instead (hook not persisted).  Triggers
 * the janitor tick periodically.
 *
 * @param bdr Cache instance.
 * @param method HTTP method; must be "GET".
 * @param path NUL-terminated request path.
 * @param data Response bytes to cache.
 * @param len Length of @p data in bytes.
 * @param fn Revalidation callback invoked on hits.
 * @param arg Opaque argument passed to @p fn.
 */
void cwist_bdr_put_revalidatable(cwist_bdr_t *bdr, const char *method, const char *path,
                                 const void *data, size_t len, cwist_bdr_revalidate_fn fn,
                                 void *arg) {
    if (!bdr || !method || !path || !data || len == 0 || !fn) return;
    if (strcmp(method, "GET") != 0) return;

    uint64_t req_h = bdr_hash(method, path);

    if (atomic_load_explicit(&bdr->is_disk_mode, memory_order_acquire)) {
        cwist_bdr_put_disk(bdr, req_h, data, len);
        return;
    }

    bdr_entry_t *entry = bdr_find_or_insert(bdr, req_h);
    if (!entry) return;

    /* Publish the hook before the blob so a reader never observes a
     * revalidatable blob without its hook. */
    entry->revalidate_arg = arg;
    atomic_store_explicit((_Atomic(cwist_bdr_revalidate_fn) *)&entry->revalidate, fn,
                          memory_order_release);

    bdr_blob_t *blob = bdr_blob_learn(data, len);
    if (!blob) return;
    atomic_store_explicit(&entry->response_hash, bdr_hash_data(data, len), memory_order_release);
    bdr_entry_publish(bdr, entry, blob);
    atomic_store_explicit(&entry->is_stable, true, memory_order_release);
    atomic_store_explicit(&entry->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->created_at, (int64_t)time(NULL), memory_order_relaxed);

    bdr_janitor_tick(bdr);
}

/**
 * @brief Update cache limits; zero arguments leave the corresponding limit unchanged.
 *
 * Takes the context mutex so the new limits are not torn against a running
 * janitor tick.
 *
 * @param bdr Cache instance.
 * @param max_bytes New byte budget; 0 keeps the current value.
 * @param max_entry_age_sec New entry TTL in seconds; 0 keeps the current value.
 * @param revalidate_hits New stability hit threshold; 0 keeps the current value.
 */
void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec,
                          uint64_t revalidate_hits) {
    if (!bdr) return;
    pthread_mutex_lock(&bdr->lock);
    if (max_bytes > 0) bdr->max_bytes = max_bytes;
    if (max_entry_age_sec > 0) bdr->max_entry_age_sec = max_entry_age_sec;
    if (revalidate_hits > 0) bdr->revalidate_hits = revalidate_hits;
    pthread_mutex_unlock(&bdr->lock);
}
