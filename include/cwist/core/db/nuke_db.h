/**
 * @file nuke_db.h
 * @brief High-Performance Read-Optimal Persistent Store using SQLite.
 *
 * Concept:
 * - Reads and writes use one stable disk-backed SQLite handle with WAL durability.
 * - An in-memory mirror is refreshed from that handle for low-overhead snapshots.
 * - Low-memory fallback releases only the mirror and never replaces the active handle.
 */

#ifndef __CWIST_NUKE_DB_H__
#define __CWIST_NUKE_DB_H__

#include <sqlite3.h>
#include <stdbool.h>

/**
 * @brief Return codes for cwist_nuke_init.
 */
#define CWIST_NUKE_OK 0
#define CWIST_NUKE_ERR_GENERIC (-1)
#define CWIST_NUKE_ERR_LOW_MEMORY (-2)

/**
 * @brief Nuke DB Context.
 *
 * Logic:
 * 1. Init: Load disk DB into Memory DB. Enables WAL mode on disk.
 * 2. Runtime:
 *    - SELECTs happen in Memory.
 *    - INSERT/UPDATE/DELETE trigger immediate background sync to Disk on COMMIT.
 * 3. Periodic: Background thread also performs periodic sync as a fail-safe.
 * 4. Exit: Catch signals (SIGINT, SIGTERM) and force final Sync.
 */
typedef struct cwist_nuke_db_t {
    sqlite3 *mem_db;     ///< Optional in-memory mirror refreshed from disk
    sqlite3 *disk_db;    ///< Stable disk-backed application handle
    char *disk_path;     ///< Path to the disk database file
    bool auto_sync;      ///< Whether auto-sync is enabled
    int sync_interval_ms;///< Sync interval in milliseconds

    bool is_disk_mode;   ///< True if running in low-memory disk fallback mode
    bool load_successful; ///< True if initial load from disk was successful
} cwist_nuke_db_t;

/**
 * @brief Initialize Nuke DB.
 *
 * Loads the disk database into memory (if it exists).
 * Sets up signal handlers for safe exit (SIGINT, SIGTERM).
 * Starts a background thread for auto-sync if interval > 0.
 *
 * @param disk_path Path to the persistent database file.
 * @param sync_interval_ms Auto-sync interval in ms (0 to disable).
 * @return 0 on success, negative on failure (see CWIST_NUKE_ERR_* codes).
 */
int cwist_nuke_init(const char *disk_path, int sync_interval_ms);

/**
 * @brief Refresh the in-memory mirror from the stable disk-backed handle.
 * @return 0 on success, -1 on failure.
 */
int cwist_nuke_sync(void);

/**
 * @brief Release the in-memory mirror while keeping the active handle stable.
 * @return 0 on success, -1 if the mirror could not be refreshed first.
 */
int cwist_nuke_force_disk_mode(void);

/**
 * @brief Close databases safely.
 * Performs a final synchronization before closing handles.
 */
void cwist_nuke_close(void);

/**
 * @brief Get the stable SQLite handle used for all application operations.
 * @return Pointer to the disk-backed sqlite3 handle, or NULL before init/after close.
 */
sqlite3 *cwist_nuke_get_db(void);

/**
 * @brief Internal signal handler.
 * Typically set up by cwist_nuke_init.
 */
void cwist_nuke_signal_handler(int signum);

/**
 * @brief Serialize the in-memory database to a raw byte buffer.
 *
 * Uses sqlite3_serialize() for direct byte-level access to the SQLite image,
 * which avoids the overhead of the backup API and enables encryption or
 * network transfer without a temporary file.
 *
 * The returned buffer is allocated by sqlite3_malloc() and must be freed
 * with sqlite3_free().  Returns NULL on failure or when NukeDB is not
 * initialised.
 *
 * @param[out] out_size Set to the number of bytes in the returned buffer.
 * @return sqlite3_malloc()-allocated byte buffer, or NULL on failure.
 */
unsigned char *cwist_nuke_serialize(sqlite3_int64 *out_size);

/**
 * @brief Replace the NukeDB contents with a raw SQLite byte buffer.
 *
 * The image is written to the database file and to the in-memory mirror, so
 * it persists like any other write. Ownership of @p data is transferred to
 * SQLite (allocated with sqlite3_malloc()) on success and on failure; do not
 * free it yourself after the call.
 *
 * @param data     Buffer previously obtained via cwist_nuke_serialize() or
 *                 cwist_db_sync_pull().  Must be sqlite3_malloc()-allocated.
 * @param data_len Length of @p data in bytes.
 * @return 0 on success, -1 on failure.
 */
int cwist_nuke_deserialize(unsigned char *data, sqlite3_int64 data_len);

#endif
