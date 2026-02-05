#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/macros.h>
#include <cwist/sys/sys_info.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

static cwist_nuke_db_t g_nuke = {0};
static pthread_t g_sync_thread;
static volatile bool g_running = false;
static pthread_mutex_t g_nuke_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t g_sigset;

// Attempt to hydrate the in-memory DB directly from the on-disk file bytes.
static int nuke_load_raw_into_memory(sqlite3_int64 *bytes_loaded) {
#if defined(SQLITE_ENABLE_DESERIALIZE)
    if (!g_nuke.disk_path) return -1;

    struct stat st;
    if (stat(g_nuke.disk_path, &st) != 0) {
        if (errno == ENOENT) return 1; // Treat missing file as new DB
        return -1;
    }

    if (st.st_size == 0) {
        return 1; // Empty file -> treat as new DB
    }

    sqlite3_int64 file_size = (sqlite3_int64)st.st_size;
    unsigned char *buffer = sqlite3_malloc64(file_size);
    if (!buffer) {
        return -1;
    }

    FILE *fp = fopen(g_nuke.disk_path, "rb");
    if (!fp) {
        sqlite3_free(buffer);
        return -1;
    }

    size_t total = 0;
    while (total < (size_t)file_size) {
        size_t chunk = fread(buffer + total, 1, (size_t)file_size - total, fp);
        if (chunk == 0) {
            if (ferror(fp)) {
                sqlite3_free(buffer);
                fclose(fp);
                return -1;
            }
            break;
        }
        total += chunk;
    }
    fclose(fp);

    if (total != (size_t)file_size) {
        sqlite3_free(buffer);
        return -1;
    }

    // SQLite takes ownership of buffer (even on failure) when FREEONCLOSE is set.
    int deserialize_flags = SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE;
    int rc = sqlite3_deserialize(g_nuke.mem_db, "main", buffer, file_size, file_size, deserialize_flags);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] sqlite3_deserialize failed: %d\n", rc);
        return -1;
    }

    if (bytes_loaded) {
        *bytes_loaded = file_size;
    }

    return 0;
#else
    CWIST_UNUSED(bytes_loaded);
    return -2;
#endif
}

// Helper: Backup source_db to dest_db
static int nuke_backup(sqlite3 *dest, sqlite3 *source) {
    if (!dest || !source) return -1;
    sqlite3_backup *backup = sqlite3_backup_init(dest, "main", source, "main");
    if (!backup) {
        return -1;
    }

    int rc = sqlite3_backup_step(backup, -1); // Copy all pages
    if (rc != SQLITE_DONE) {
        // fprintf(stderr, "[NukeDB] Backup step failed: %d\n", rc);
    }

    sqlite3_backup_finish(backup);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static void nuke_switch_to_disk(void) {
    if (g_nuke.is_disk_mode) return;

    printf("[NukeDB] CRITICAL: Low RAM detected (%zu bytes). Switching to Disk DB.\n", (size_t)cwist_get_available_ram());
    
    pthread_mutex_lock(&g_nuke_lock);
    
    // 1. Flush Memory -> Disk
    nuke_backup(g_nuke.disk_db, g_nuke.mem_db);
    
    // 2. Set mode flag
    sqlite3_db_release_memory(g_nuke.mem_db); 
    
    g_nuke.is_disk_mode = true;
    
    pthread_mutex_unlock(&g_nuke_lock);
}

// Internal commit hook to trigger immediate sync
static int nuke_commit_hook(void *arg) {
    CWIST_UNUSED(arg);
    if (g_running && !g_nuke.is_disk_mode) {
        // Send signal to wake up sync thread
        pthread_kill(g_sync_thread, SIGUSR2);
    }
    return 0;
}

int cwist_nuke_sync(void) {
    pthread_mutex_lock(&g_nuke_lock);
    
    // Safety: If initial load failed, don't overwrite disk with empty memory DB
    // unless this is a new file (which we'd have to track)
    if (!g_nuke.load_successful) {
        pthread_mutex_unlock(&g_nuke_lock);
        return -1;
    }

    if (g_nuke.is_disk_mode) {
        sqlite3_wal_checkpoint_v2(g_nuke.disk_db, "main", SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        pthread_mutex_unlock(&g_nuke_lock);
        return 0;
    }
    
    if (!g_nuke.mem_db || !g_nuke.disk_db) {
        pthread_mutex_unlock(&g_nuke_lock);
        return -1;
    }
    
    int rc = nuke_backup(g_nuke.disk_db, g_nuke.mem_db);
    pthread_mutex_unlock(&g_nuke_lock);
    return rc;
}

// Internal idempotent cleanup
static void nuke_cleanup_internal(void) {
    pthread_mutex_lock(&g_nuke_lock);
    
    // Close Memory DB
    if (g_nuke.mem_db) {
        sqlite3_close(g_nuke.mem_db);
        g_nuke.mem_db = NULL;
    }

    // Close Disk DB
    if (g_nuke.disk_db) {
        sqlite3_close(g_nuke.disk_db);
        g_nuke.disk_db = NULL;
    }

    if (g_nuke.disk_path) {
        free(g_nuke.disk_path);
        g_nuke.disk_path = NULL;
    }

    pthread_mutex_unlock(&g_nuke_lock);
}

void cwist_nuke_close(void) {
    if (!g_running) return;
    g_running = false;

    // Wake up the sync thread if it's sleeping/waiting
    if (g_sync_thread) {
        pthread_kill(g_sync_thread, SIGUSR1);
        pthread_join(g_sync_thread, NULL);
    }

    printf("[NukeDB] Saving data before exit...\n");
    cwist_nuke_sync();
    nuke_cleanup_internal();
    
    printf("[NukeDB] Closed safely.\n");
}

static void *sync_thread_func(void *arg) {
    CWIST_UNUSED(arg);
    struct timespec timeout;
    int signum;

    while (g_running) {
        // Calculate timeout for periodic sync
        // Note: sigtimedwait uses relative timeout? No, struct timespec is usually absolute for sem_timedwait 
        // but sigtimedwait uses RELATIVE timeout interval.
        timeout.tv_sec = g_nuke.sync_interval_ms / 1000;
        timeout.tv_nsec = (g_nuke.sync_interval_ms % 1000) * 1000000;

        // Wait for signals (INT, TERM, USR1, USR2) or Timeout
        signum = sigtimedwait(&g_sigset, NULL, &timeout);

        if (signum > 0) {
            if (signum == SIGUSR1) {
                // Requested to stop via cwist_nuke_close
                break;
            }

            if (signum == SIGUSR2) {
                // Immediate sync requested via commit hook
                cwist_nuke_sync();
                continue;
            }
            
            // Handle termination signals (SIGINT, SIGTERM)
            printf("\n[NukeDB] Intercepted Signal %d. Saving and shutting down...\n", signum);
            
            // 1. Force Sync
            cwist_nuke_sync();
            
            // 2. Cleanup resources
            nuke_cleanup_internal();
            g_running = false;

            // 3. Re-raise the signal to allow default handling (and proper exit code)
            // Unblock the signal first
            sigset_t s;
            sigemptyset(&s);
            sigaddset(&s, signum);
            pthread_sigmask(SIG_UNBLOCK, &s, NULL);
            
            // Restore default handler
            signal(signum, SIG_DFL);
            
            // Send delayed signal to self
            raise(signum);
            return NULL;
        } else { // Timeout (EAGAIN) or Interruption
            if (errno == EAGAIN && g_running && g_nuke.auto_sync) {
                // RAM Check
                if (cwist_is_ram_critical(CWIST_MIB(128))) {
                    nuke_switch_to_disk();
                } else {
                    cwist_nuke_sync();
                }
            }
        }
    }
    return NULL;
}

int cwist_nuke_init(const char *disk_path, int sync_interval_ms) {
    if (g_running) return -1; // Already running

    g_nuke.disk_path = strdup(disk_path);
    g_nuke.sync_interval_ms = sync_interval_ms > 0 ? sync_interval_ms : 1000; // Default 1s if 0
    g_nuke.auto_sync = (sync_interval_ms > 0);
    g_nuke.is_disk_mode = false;

    // 1. Open Disk DB
    int rc = sqlite3_open(disk_path, &g_nuke.disk_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open disk DB: %s\n", sqlite3_errmsg(g_nuke.disk_db));
        return -1;
    }

    // Set WAL mode for robustness and performance
    sqlite3_exec(g_nuke.disk_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_nuke.disk_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    // 2. Open Memory DB
    rc = sqlite3_open(":memory:", &g_nuke.mem_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open memory DB: %s\n", sqlite3_errmsg(g_nuke.mem_db));
        sqlite3_close(g_nuke.disk_db);
        return -1;
    }

    // 3. Load Disk -> Memory
    printf("[NukeDB] Loading data from disk to memory (Full Copy)...\n");
    pthread_mutex_lock(&g_nuke_lock);
    sqlite3_int64 raw_bytes = 0;
    int raw_rc = nuke_load_raw_into_memory(&raw_bytes);
    if (raw_rc == 0) {
        printf("[NukeDB] Raw SQLite image %lld bytes loaded directly into RAM.\n", (long long)raw_bytes);
        g_nuke.load_successful = true;
    } else if (raw_rc == 1) {
        printf("[NukeDB] New or empty database detected. Initializing Memory-Master mode.\n");
        g_nuke.load_successful = true;
    } else {
        if (raw_rc == -2) {
            printf("[NukeDB] Raw deserialize disabled at build time. Falling back to sqlite3_backup.\n");
        }

        int backup_rc = nuke_backup(g_nuke.mem_db, g_nuke.disk_db);
        if (backup_rc == 0) {
            g_nuke.load_successful = true;
        } else {
            // If load failed, it might be a brand new file (0 bytes).
            sqlite3_stmt *stmt;
            int has_tables = 0;
            if (sqlite3_prepare_v2(g_nuke.disk_db, "SELECT count(*) FROM sqlite_master WHERE type='table';", -1, &stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    has_tables = sqlite3_column_int(stmt, 0) > 0;
                }
                sqlite3_finalize(stmt);
            }

            if (!has_tables) {
                printf("[NukeDB] New or empty database detected. Initializing Memory-Master mode.\n");
                g_nuke.load_successful = true;
            } else {
                fprintf(stderr, "[NukeDB] Initial load failed for existing database. Synchronization to disk disabled.\n");
                g_nuke.load_successful = false;
            }
        }
    }
    pthread_mutex_unlock(&g_nuke_lock);

    // Register commit hook for immediate sync
    sqlite3_commit_hook(g_nuke.mem_db, nuke_commit_hook, NULL);

    g_running = true;

    // 4. Setup Signal Interception (Block signals in main thread)
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    sigaddset(&g_sigset, SIGUSR1); // Used for internal thread wake-up
    sigaddset(&g_sigset, SIGUSR2); // Used for immediate sync trigger
    
    // This blocks these signals for the calling thread (main) and any subsequently created threads.
    // The sync_thread will consume them via sigtimedwait.
    int s_rc = pthread_sigmask(SIG_BLOCK, &g_sigset, NULL);
    if (s_rc != 0) {
        fprintf(stderr, "[NukeDB] Failed to block signals: %d\n", s_rc);
    }

    // 5. Start Sync/Signal Thread
    pthread_create(&g_sync_thread, NULL, sync_thread_func, NULL);

    // 6. Register atexit for ultimate safety
    atexit(cwist_nuke_close);

    return 0;
}

sqlite3 *cwist_nuke_get_db(void) {
    // Force return memory handle to ensure "Entirely in Memory" rule
    if (g_nuke.is_disk_mode) return g_nuke.disk_db;
    return g_nuke.mem_db;
}

// Deprecated: No longer used with the sigwait model, but kept for symbol compatibility if needed
void cwist_nuke_signal_handler(int signum) {
    CWIST_UNUSED(signum);
    // No-op
}
