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

static cwist_nuke_db_t g_nuke = {0};
static pthread_t g_sync_thread;
static volatile bool g_running = false;
static pthread_mutex_t g_nuke_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t g_sigset;

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

int cwist_nuke_sync(void) {
    pthread_mutex_lock(&g_nuke_lock);
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

        // Wait for signals (INT, TERM, USR1) or Timeout
        signum = sigtimedwait(&g_sigset, NULL, &timeout);

        if (signum > 0) {
            if (signum == SIGUSR1) {
                // Requested to stop via cwist_nuke_close
                break;
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
    }

    g_running = true;

    // 4. Setup Signal Interception (Block signals in main thread)
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    sigaddset(&g_sigset, SIGUSR1); // Used for internal thread wake-up
    
    // This blocks these signals for the calling thread (main) and any subsequently created threads.
    // The sync_thread will consume them via sigtimedwait.
    int s_rc = pthread_sigmask(SIG_BLOCK, &g_sigset, NULL);
    if (s_rc != 0) {
        fprintf(stderr, "[NukeDB] Failed to block signals: %d\n", s_rc);
    }

    // 5. Start Sync/Signal Thread
    pthread_create(&g_sync_thread, NULL, sync_thread_func, NULL);

    return 0;
}

sqlite3 *cwist_nuke_get_db(void) {
    if (g_nuke.is_disk_mode) return g_nuke.disk_db;
    return g_nuke.mem_db;
}

// Deprecated: No longer used with the sigwait model, but kept for symbol compatibility if needed
void cwist_nuke_signal_handler(int signum) {
    CWIST_UNUSED(signum);
    // No-op
}