#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/big_dumb_reply.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <cwist/vendor/sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BDR_BUCKETS 1024

// SipHash key for BDR (Hardcoded or random at startup)
static const uint8_t BDR_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

cwist_bdr_t *cwist_bdr_create(void) {
    cwist_bdr_t *bdr = calloc(1, sizeof(cwist_bdr_t));
    if (!bdr) return NULL;
    bdr->bucket_count = BDR_BUCKETS;
    bdr->buckets = calloc(BDR_BUCKETS, sizeof(bdr_entry_t *));
    bdr->latency_threshold_ms = 10; 
    bdr->disk_db = NULL;
    bdr->is_disk_mode = false;
    return bdr;
}

void cwist_bdr_destroy(cwist_bdr_t *bdr) {
    if (!bdr) return;
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
    free(bdr);
}

static uint64_t bdr_hash(const char *method, const char *path) {
    uint64_t h = siphash24((const void*)path, strlen(path), BDR_KEY);
    h ^= (uint64_t)(method[0]); 
    return h;
}

static void bdr_check_ram(cwist_bdr_t *bdr) {
    if (bdr->is_disk_mode) return;
    
    // Threshold: 64MB free (conservative)
    if (cwist_is_ram_critical(CWIST_MIB(64))) {
        printf("[BDR] Low RAM. Switching to Disk Cache.\n");
        // Open Disk DB
        if (sqlite3_open("cwist_bdr_fallback.db", &bdr->disk_db) == SQLITE_OK) {
             char *err = NULL;
             sqlite3_exec(bdr->disk_db, "CREATE TABLE IF NOT EXISTS bdr (hash INTEGER PRIMARY KEY, blob BLOB);", NULL, NULL, &err);
             if (err) sqlite3_free(err);
             
             // Move existing memory items to disk
             sqlite3_exec(bdr->disk_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
             for (size_t i = 0; i < bdr->bucket_count; i++) {
                bdr_entry_t *curr = bdr->buckets[i];
                while (curr) {
                    if (curr->is_stable) { // Only move stable items
                        sqlite3_stmt *stmt;
                        sqlite3_prepare_v2(bdr->disk_db, "INSERT INTO bdr (hash, blob) VALUES (?, ?);", -1, &stmt, NULL);
                        sqlite3_bind_int64(stmt, 1, curr->request_hash);
                        sqlite3_bind_blob(stmt, 2, curr->response_blob, curr->len, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                    }
                    
                    // Free memory
                    bdr_entry_t *next = curr->next;
                    free(curr->response_blob);
                    free(curr);
                    curr = next;
                }
                bdr->buckets[i] = NULL;
             }
             sqlite3_exec(bdr->disk_db, "COMMIT;", NULL, NULL, NULL);
             bdr->is_disk_mode = true;
        }
    }
}

static uint64_t bdr_hash_data(const void *data, size_t len) {

    return siphash24(data, len, BDR_KEY);

}



const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len) {

    if (!bdr || !method || !path) return NULL;

    if (strcmp(method, "GET") != 0) return NULL;



    uint64_t h = bdr_hash(method, path);



    // Disk Mode

    if (bdr->is_disk_mode) {

        // Disk mode logic remains same (Fail-safe: return NULL or implement read)

        // For now, we stick to "Write-only" on disk for safety as per previous step.

        return NULL; 

    }



    // Memory Mode

    size_t idx = h % bdr->bucket_count;

    bdr_entry_t *curr = bdr->buckets[idx];

    while (curr) {

        if (curr->request_hash == h) {

            curr->hits++;

            if (curr->is_stable && curr->response_blob) {

                if (out_len) *out_len = curr->len;

                return curr->response_blob;

            }

            return NULL; // Found but not stable yet

        }

        curr = curr->next;

    }

    return NULL;

}



void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len) {

    if (!bdr || !method || !path || !data || len == 0) return;

    if (strcmp(method, "GET") != 0) return;



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

            return;

        }

        curr = curr->next;

    }

    

    // New Entry (Candidate)

    bdr_entry_t *entry = malloc(sizeof(bdr_entry_t));

    if (!entry) return;



    entry->request_hash = req_h;

    entry->response_hash = res_h;

    entry->is_stable = false; // Start as candidate

    entry->response_blob = NULL;

    entry->len = 0;

    entry->hits = 0;

    entry->created_at = time(NULL);

    

    entry->next = bdr->buckets[idx];

    bdr->buckets[idx] = entry;

}
