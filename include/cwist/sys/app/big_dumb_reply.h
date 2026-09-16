/**
 * @file big_dumb_reply.h
 * @brief Auto-Caching Layer for repetitive requests.
 */

#ifndef __CWIST_BIG_DUMB_REPLY_H__
#define __CWIST_BIG_DUMB_REPLY_H__

#include <cwist/core/sstring/sstring.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/**
 * @brief Refcount-free cached response blob, reclaimed via libttak EBR.
 *
 * Readers hold the blob between cwist_bdr_get_pinned() and
 * cwist_bdr_unpin(); writers replace it with a single atomic exchange and
 * retire the predecessor across an epoch boundary, so a swapped-out blob is
 * never freed while a reader still serves it.  @p mem is the backing
 * allocation released with @p free_fn (cwist_free for internally learned
 * blobs, a user-supplied releaser for callback-provided buffers).
 */
typedef struct bdr_blob_t {
    void *mem;               ///< Backing allocation (this object for learned blobs).
    void (*free_fn)(void *); ///< Releaser for @p mem.
    size_t len;              ///< Length of @p data in bytes.
    const unsigned char *data; ///< Serialized HTTP response (headers + body).
} bdr_blob_t;

/**
 * @brief Revalidation callback attached to a cache entry.
 *
 * Invoked on cache hits for entries registered through
 * cwist_bdr_put_revalidatable().  Return false to keep serving the cached
 * blob.  Return true when the underlying value changed: @p *out_data must
 * then point to the freshly serialized response (ownership moves to the
 * cache — zero copy, only the pointer is swung), @p *out_len to its length,
 * and @p *out_free to the releaser for that buffer (NULL means cwist_free).
 */
typedef bool (*cwist_bdr_revalidate_fn)(void *arg, void **out_data, size_t *out_len,
                                        void (**out_free)(void *));

/**
 * @brief Big Dumb Reply Entry.
 * Stores a completely serialized HTTP response blob.
 */
typedef struct bdr_entry_t {
    uint64_t request_hash; ///< Key: SipHash(Method + Path)
    
    uint64_t response_hash;///< Hash of the response content (for stability check)
    bool is_stable;        ///< True if response proved stable across requests
    
    void *response_blob;   ///< Complete HTTP response (headers + body)
    size_t len;            ///< Length of blob
    
    uint64_t hits;         ///< Hit count
    time_t created_at;     ///< Creation timestamp
    
    struct bdr_entry_t *next;
} bdr_entry_t;

/**
 * @brief Per-connection cursor that turns repeated hits of the same route
 * into a content-compare instead of a SipHash + bucket walk.
 *
 * Keep-alive clients overwhelmingly repeat the same few routes; the cursor
 * remembers the last served entry and its path, so the next lookup on the
 * same connection only memcmps the path (usually a handful of bytes) and
 * re-validates the entry state under the EBR epoch exactly like a full
 * lookup.  It holds no ownership: entries are tombstoned rather than freed,
 * and every use re-checks retirement/stability, so a stale cursor degrades
 * to a normal miss, never to a wrong serve.
 *
 * Embed by value (zero-initialized) in per-connection state.
 */
#define CWIST_BDR_CURSOR_PATH_MAX 128
typedef struct cwist_bdr_cursor {
    bdr_entry_t *entry;   ///< Last served entry (not owned; validated per use)
    uint64_t req_hash;    ///< request_hash of @p entry
    size_t path_len;      ///< Length of @p path
    char path[CWIST_BDR_CURSOR_PATH_MAX]; ///< Last served path bytes
} cwist_bdr_cursor_t;

/**
 * @brief Big Dumb Reply Context.
 * Manages cache buckets and learning parameters.
 */
typedef struct cwist_bdr_t {
    pthread_mutex_t lock;      ///< Serializes cache entry and blob lifetime changes.
    bdr_entry_t **buckets;     ///< Hash buckets
    size_t bucket_count;       ///< Number of buckets
    
    // Learning Config
    int hit_threshold;         ///< (Unused) Hits before caching
    int latency_threshold_ms;  ///< Latency threshold to trigger caching
    
    // Fallback Disk DB
    struct sqlite3 *disk_db;   ///< Disk DB handle for low-RAM mode
    bool is_disk_mode;         ///< True if fallback is active
} cwist_bdr_t;

/**
 * @brief Creates a BDR context.
 */
cwist_bdr_t *cwist_bdr_create(void);

/**
 * @brief Destroys a BDR context and frees all cached blobs.
 */
void cwist_bdr_destroy(cwist_bdr_t *bdr);

/**
 * @brief Try to find a cached response.
 * @param bdr Context.
 * @param method HTTP Method (only GET supported).
 * @param path Request path.
 * @param out_len [out] Length of the found blob.
 * @return Pointer to the blob if found, NULL otherwise.
 */
const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len);

/**
 * @brief Copy a stable response from the cache for concurrent server use.
 *
 * The caller owns the returned buffer and must release it with cwist_free().
 */
void *cwist_bdr_copy_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len);

/**
 * @brief Store a response in the cache.
 * @param bdr Context.
 * @param method HTTP Method.
 * @param path Request path.
 * @param data Serialized response data.
 * @param len Length of data.
 */
void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data,
                   size_t len);

/**
 * @brief Immediately cache a fixed static response on request 1.
 */
void cwist_bdr_put_fixed(cwist_bdr_t *bdr, const char *method, const char *path, const void *data,
                         size_t len);

/**
 * @brief Cache a response whose backing value may change over time.
 *
 * Behaves like cwist_bdr_put_fixed() (stable from the first request) but
 * attaches @p fn to the entry.  Every cache hit invokes @p fn first; when it
 * reports a change, the new buffer it supplies is published with a single
 * pointer exchange — no memcpy, no global lock — and the old blob is
 * retired across an epoch boundary.
 */
void cwist_bdr_put_revalidatable(cwist_bdr_t *bdr, const char *method, const char *path,
                                 const void *data, size_t len, cwist_bdr_revalidate_fn fn,
                                 void *arg);

/**
 * @brief Adjusts guard-rail policies for the in-memory cache.
 * @param bdr Context.
 * @param max_bytes Maximum bytes to keep in RAM (0 keeps default).
 * @param max_entry_age_sec Time-to-live for cached entries (<=0 keeps default).
 * @param revalidate_hits Force relearning after this many hits (0 keeps default).
 */
void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec,
                          uint64_t revalidate_hits);

#endif
