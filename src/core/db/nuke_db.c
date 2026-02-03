#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/macros.h>
#include <cwist/sys/sys_info.h>
#include <cwist/vendor/sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

static cwist_nuke_db_t g_nuke = {0};
static pthread_t g_sync_thread;
static volatile bool g_running = false;
static pthread_mutex_t g_nuke_lock = PTHREAD_MUTEX_INITIALIZER;

// Helper: Backup source_db to dest_db
static int nuke_backup(sqlite3 *dest, sqlite3 *source) {
    if (!dest || !source) return -1;
    sqlite3_backup *backup = sqlite3_backup_init(dest, "main", source, "main");
    if (!backup) {
        // fprintf(stderr, "[NukeDB] Backup init failed: %s\n", sqlite3_errmsg(dest));
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
    
    // 2. Set mode flag (Readers will now get disk_db)
    // Note: We cannot safely close mem_db yet as other threads might hold the pointer.
    // We intentionally leak the mem_db connection handle for the session duration 
    // or until we can implement safe refcounting.
    // However, SQLite memory usually is freed when the connection closes.
    // For now, we keep it open but unused to prevent segfaults.
    // To actually free RAM, we need to instruct SQLite to shrink memory.
    
    sqlite3_db_release_memory(g_nuke.mem_db); 
    
    g_nuke.is_disk_mode = true;
    
    pthread_mutex_unlock(&g_nuke_lock);
}

int cwist_nuke_sync(void) {
    pthread_mutex_lock(&g_nuke_lock);
    if (g_nuke.is_disk_mode) {
        // Already on disk, nothing to sync from memory.
        // Maybe WAL checkpoint?
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

void cwist_nuke_close(void) {
    if (!g_running) return;
    g_running = false;

    if (g_nuke.auto_sync && g_sync_thread) {
        pthread_join(g_sync_thread, NULL);
    }

    printf("[NukeDB] Saving data before exit...\n");
    cwist_nuke_sync();

    pthread_mutex_lock(&g_nuke_lock);
    if (g_nuke.mem_db) sqlite3_close(g_nuke.mem_db);
    if (g_nuke.disk_db) sqlite3_close(g_nuke.disk_db);
    if (g_nuke.disk_path) free(g_nuke.disk_path);

    g_nuke.mem_db = NULL;
    g_nuke.disk_db = NULL;
    g_nuke.disk_path = NULL;
    pthread_mutex_unlock(&g_nuke_lock);
    
    printf("[NukeDB] Closed safely.\n");
}

void cwist_nuke_signal_handler(int signum) {
    CWIST_UNUSED(signum);
    printf("\n[NukeDB] Signal received. Shutting down...\n");
    cwist_nuke_close();
    exit(0);
}

static void *sync_thread_func(void *arg) {
    CWIST_UNUSED(arg);
    while (g_running) {
        usleep(g_nuke.sync_interval_ms * 1000);
        if (!g_running) break;
        
        // Monitor RAM (Threshold: 128MB for example)
        if (cwist_is_ram_critical(CWIST_MIB(128))) {
            nuke_switch_to_disk();
        } else {
            cwist_nuke_sync();
        }
    }
    return NULL;
}

int cwist_nuke_init(const char *disk_path, int sync_interval_ms) {
    if (g_running) return -1; // Already running

    g_nuke.disk_path = strdup(disk_path);
    g_nuke.sync_interval_ms = sync_interval_ms;
    g_nuke.auto_sync = (sync_interval_ms > 0);
    g_nuke.is_disk_mode = false;

    // 1. Open Disk DB
    int rc = sqlite3_open(disk_path, &g_nuke.disk_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open disk DB: %s\n", sqlite3_errmsg(g_nuke.disk_db));
        return -1;
    }

    // 2. Open Memory DB
    rc = sqlite3_open(":memory:", &g_nuke.mem_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[NukeDB] Failed to open memory DB: %s\n", sqlite3_errmsg(g_nuke.mem_db));
        return -1;
    }

    // 3. Load Disk -> Memory
    printf("[NukeDB] Loading data from disk...\n");
    if (nuke_backup(g_nuke.mem_db, g_nuke.disk_db) != 0) {
        fprintf(stderr, "[NukeDB] Initial load failed. Starting with empty memory DB.\n");
        // Not fatal, might be fresh DB
    }

    g_running = true;

    // 4. Setup Signal Handlers
    struct sigaction sa;
    sa.sa_handler = cwist_nuke_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 5. Start Sync Thread
    if (g_nuke.auto_sync) {
        pthread_create(&g_sync_thread, NULL, sync_thread_func, NULL);
    }

    return 0;
}

sqlite3 *cwist_nuke_get_db(void) {
    // Thread-safe fetch of the current active handle
    // Note: If using NukeDB, applications should get the handle per request
    // and not cache it, because it might switch to disk_db on low memory.
    if (g_nuke.is_disk_mode) return g_nuke.disk_db;
    return g_nuke.mem_db;
}
