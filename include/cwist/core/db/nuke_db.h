/**
 * @file nuke_db.h
 * @brief High-Performance In-Memory SQLite with Persistent Disk Sync.
 */

#ifndef __CWIST_NUKE_DB_H__
#define __CWIST_NUKE_DB_H__

#include <sqlite3.h>
#include <stdbool.h>

/**
 * @brief Nuke DB Context.
 * 
 * Logic:
 * 1. Init: Load disk DB into Memory DB.
 * 2. Runtime: All operations happen in Memory DB (Fast).
 * 3. Sync: Periodically (or manually) backup Memory DB to Disk DB.
 * 4. Exit: Catch signals (SIGINT, SIGTERM) and force Sync.
 */
typedef struct cwist_nuke_db_t {
    sqlite3 *mem_db;     ///< The active in-memory database handle
    sqlite3 *disk_db;    ///< The backup disk database handle
    char *disk_path;     ///< Path to the disk database file
    bool auto_sync;      ///< Whether auto-sync is enabled
    int sync_interval_ms;///< Sync interval in milliseconds
    
    bool is_disk_mode;   ///< True if running in low-memory disk fallback mode
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
 * @return 0 on success, -1 on failure.
 */
int cwist_nuke_init(const char *disk_path, int sync_interval_ms);

/**
 * @brief Force synchronization from Memory to Disk.
 * @return 0 on success, -1 on failure.
 */
int cwist_nuke_sync(void);

/**
 * @brief Close databases safely.
 * Performs a final synchronization before closing handles.
 */
void cwist_nuke_close(void);

/**
 * @brief Get the raw SQLite3 handle for the In-Memory DB.
 * @return Pointer to the active sqlite3* handle.
 */
sqlite3 *cwist_nuke_get_db(void);

/**
 * @brief Internal signal handler.
 * Typically set up by cwist_nuke_init.
 */
void cwist_nuke_signal_handler(int signum);

#endif
