/** @file log_async.c
 * @brief Opt-in background writer for log lines (cwist_log_async_start()).
 *
 * Each output fd (stdout, stderr) has one byte ring. A caller formats its
 * line on its own stack, copies it into the ring under one mutex and
 * returns; a single writer thread write()s the rings to their fds. Callers
 * never make a syscall on the request path, and lines stay whole and in
 * submission order per fd.
 *
 * A full ring makes the caller wait for space, so no line is dropped. A line
 * larger than the ring, or any line while the writer is not running, is
 * refused and the caller writes it synchronously as before.
 *
 * fork(): the child keeps the rings' memory but not the writer thread. Its
 * copy of the parent's queued bytes is discarded (the parent writes them)
 * and a writer is started on the child's first line.
 */
#define _POSIX_C_SOURCE 200809L
#include "log_async.h"
#include <cwist/sys/app/logger.h>
#include <cwist/core/mem/alloc.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__wasm__)
/* WASM builds have no fork() and, without the threads proposal, no
 * background thread: async logging never starts and every line is written
 * synchronously. */
bool cwist_log_async_start(size_t buffer_bytes) {
    (void)buffer_bytes;
    return false;
}
void cwist_log_async_flush(void) {}
void cwist_log_async_stop(void) {}
bool cwist_log_async_active(void) {
    return false;
}
bool cwist_log_async_submit(int fd, const char *line, size_t len) {
    (void)fd;
    (void)line;
    (void)len;
    return false;
}
#else

#define LOG_ASYNC_DEFAULT_BYTES ((size_t)256 * 1024)
#define LOG_ASYNC_STREAMS 2 /* [0] = stdout, [1] = stderr */

typedef struct {
    char *buf;
    size_t cap;
    size_t head; /* total bytes queued; guarded by g_lock */
    size_t tail; /* total bytes written; guarded by g_lock */
} log_ring_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;  /* writer waits for bytes */
static pthread_cond_t g_space = PTHREAD_COND_INITIALIZER; /* callers wait for space or a drain */
static log_ring_t g_rings[LOG_ASYNC_STREAMS];
static bool g_active;        /* accepting lines */
static bool g_stopping;      /* writer drains, then exits */
static bool g_writer_alive;  /* g_writer is a live thread in this process */
static pthread_t g_writer;
static bool g_hooks_installed;

/** @brief Map an fd to its ring, or NULL for an fd the writer does not own. */
static log_ring_t *ring_for_fd(int fd) {
    if (fd == STDOUT_FILENO) return &g_rings[0];
    if (fd == STDERR_FILENO) return &g_rings[1];
    return NULL;
}

/** @brief True when every ring has been written out. Caller holds g_lock. */
static bool rings_empty(void) {
    for (int i = 0; i < LOG_ASYNC_STREAMS; i++) {
        if (g_rings[i].head != g_rings[i].tail) return false;
    }
    return true;
}

/** @brief write() all of @p len bytes, retrying on EINTR and short writes.
 * Gives up on any other error so a closed fd cannot wedge the writer. */
static void write_all(int fd, const char *p, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

/** @brief Writer thread: write each ring's queued bytes, until stopped and drained.
 * The bytes are written with g_lock released; tail only advances after the
 * write, so the region stays reserved and a flush waits for it. */
static void *log_writer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (rings_empty() && !g_stopping) pthread_cond_wait(&g_work, &g_lock);
        if (rings_empty()) break; /* stopping and drained */

        for (int i = 0; i < LOG_ASYNC_STREAMS; i++) {
            log_ring_t *r = &g_rings[i];
            while (r->head != r->tail) {
                size_t off = r->tail % r->cap;
                size_t len = r->head - r->tail;
                if (len > r->cap - off) len = r->cap - off; /* up to the wrap */
                const char *p = r->buf + off;
                pthread_mutex_unlock(&g_lock);
                write_all(i == 0 ? STDOUT_FILENO : STDERR_FILENO, p, len);
                pthread_mutex_lock(&g_lock);
                r->tail += len;
                pthread_cond_broadcast(&g_space);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/** @brief Start the writer thread. Caller holds g_lock.
 * @return true when the writer is running. */
static bool spawn_writer(void) {
    if (g_writer_alive) return true;
    if (pthread_create(&g_writer, NULL, log_writer_main, NULL) != 0) return false;
    g_writer_alive = true;
    return true;
}

/** @brief pthread_atfork prepare: hold g_lock so the child sees consistent rings. */
static void atfork_prepare(void) {
    pthread_mutex_lock(&g_lock);
}

/** @brief pthread_atfork parent: release g_lock. */
static void atfork_parent(void) {
    pthread_mutex_unlock(&g_lock);
}

/** @brief pthread_atfork child: drop the parent's queued bytes and the dead writer.
 * The parent writes those bytes itself; the child starts its own writer on
 * its first line. */
static void atfork_child(void) {
    for (int i = 0; i < LOG_ASYNC_STREAMS; i++) g_rings[i].tail = g_rings[i].head;
    g_writer_alive = false;
    /* Waiters of the parent do not exist here; start the conditions clean. */
    pthread_cond_t fresh = PTHREAD_COND_INITIALIZER;
    g_work = fresh;
    g_space = fresh;
    pthread_mutex_unlock(&g_lock);
}

/** @brief atexit hook: write out what is still queued before the process exits. */
static void atexit_stop(void) {
    cwist_log_async_stop();
}

/** @brief Start writing log lines from a background thread.
 * @param buffer_bytes Ring size per stream (stdout and stderr); 0 selects 256 KiB.
 * @return true when async logging is active (also when it already was),
 *         false when memory or the thread could not be had; logging then
 *         stays synchronous.
 * @note Covers cwist_logger_* and the built-in access-log middleware. Lines
 *       queued at exit() are written by an atexit hook; call
 *       cwist_log_async_stop() or cwist_log_async_flush() before _exit() or a
 *       crash-prone section if the last lines matter.
 */
bool cwist_log_async_start(size_t buffer_bytes) {
    if (buffer_bytes == 0) buffer_bytes = LOG_ASYNC_DEFAULT_BYTES;

    pthread_mutex_lock(&g_lock);
    if (g_active) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    if (!g_hooks_installed) {
        if (pthread_atfork(atfork_prepare, atfork_parent, atfork_child) != 0 ||
            atexit(atexit_stop) != 0) {
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        g_hooks_installed = true;
    }
    for (int i = 0; i < LOG_ASYNC_STREAMS; i++) {
        log_ring_t *r = &g_rings[i];
        r->buf = cwist_alloc(buffer_bytes);
        if (!r->buf) {
            for (int j = 0; j < i; j++) {
                cwist_free(g_rings[j].buf);
                g_rings[j].buf = NULL;
            }
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        r->cap = buffer_bytes;
        r->head = r->tail = 0;
    }
    g_stopping = false;
    if (!spawn_writer()) {
        for (int i = 0; i < LOG_ASYNC_STREAMS; i++) {
            cwist_free(g_rings[i].buf);
            g_rings[i].buf = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g_active = true;
    pthread_mutex_unlock(&g_lock);
    return true;
}

/** @brief Block until every line queued so far has been written. No-op when inactive. */
void cwist_log_async_flush(void) {
    pthread_mutex_lock(&g_lock);
    while (g_active && g_writer_alive && !rings_empty()) pthread_cond_wait(&g_space, &g_lock);
    pthread_mutex_unlock(&g_lock);
}

/** @brief Write out every queued line, stop the writer and go back to synchronous logging.
 * Safe to call when async logging is not active, and more than once. */
void cwist_log_async_stop(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_active) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_active = false; /* new lines go synchronous from here on */
    g_stopping = true;
    bool join = g_writer_alive;
    pthread_cond_broadcast(&g_work);
    pthread_cond_broadcast(&g_space); /* callers waiting for space fall back */
    pthread_mutex_unlock(&g_lock);

    if (join) pthread_join(g_writer, NULL);

    pthread_mutex_lock(&g_lock);
    g_writer_alive = false;
    for (int i = 0; i < LOG_ASYNC_STREAMS; i++) {
        cwist_free(g_rings[i].buf);
        g_rings[i].buf = NULL;
        g_rings[i].cap = g_rings[i].head = g_rings[i].tail = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

/** @brief Report whether async logging is active.
 * @return true between a successful cwist_log_async_start() and cwist_log_async_stop(). */
bool cwist_log_async_active(void) {
    pthread_mutex_lock(&g_lock);
    bool active = g_active;
    pthread_mutex_unlock(&g_lock);
    return active;
}

bool cwist_log_async_submit(int fd, const char *line, size_t len) {
    if (len == 0) return true;
    pthread_mutex_lock(&g_lock);
    log_ring_t *r = ring_for_fd(fd);
    if (!g_active || !r || len > r->cap) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!g_writer_alive && !spawn_writer()) { /* first line in a forked child */
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    while (g_active && r->cap - (r->head - r->tail) < len) pthread_cond_wait(&g_space, &g_lock);
    if (!g_active) { /* stopped while waiting */
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    size_t off = r->head % r->cap;
    size_t first = r->cap - off;
    if (first > len) first = len;
    memcpy(r->buf + off, line, first);
    memcpy(r->buf, line + first, len - first);
    bool was_empty = rings_empty();
    r->head += len;
    if (was_empty) pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return true;
}

#endif /* !__wasm__ */
