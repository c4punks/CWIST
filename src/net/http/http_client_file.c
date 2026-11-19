#define _POSIX_C_SOURCE 200809L
#include "http_client_file.h"

#include "async_gc.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/sys/io/cwist_io.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    cwist_io_queue *queue;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char *path;
    cwist_http_method_t method;
    cwist_http_response *response;
    bool done;
    bool detached;
} file_job;

static int file_cond_init(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return -1;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
#endif
    int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

static void file_deadline(struct timespec *deadline, int timeout_ms) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    clock_gettime(CLOCK_MONOTONIC, deadline);
#else
    clock_gettime(CLOCK_REALTIME, deadline);
#endif
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int file_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *file_url_path(const char *url) {
    const char *authority = url + strlen("file://");
    const char *path = strchr(authority, '/');
    if (!path || (authority != path &&
                  (path - authority != 9 || strncasecmp(authority, "localhost", 9) != 0))) {
        return NULL;
    }
    size_t len = strlen(path);
    char *decoded = cwist_alloc(len + 1);
    if (!decoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '?' || c == '#') {
            cwist_free(decoded);
            return NULL;
        }
        if (c == '%') {
            if (i + 2 >= len) {
                cwist_free(decoded);
                return NULL;
            }
            int hi = file_hex((unsigned char)path[i + 1]);
            int lo = file_hex((unsigned char)path[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                cwist_free(decoded);
                return NULL;
            }
            decoded[out++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            decoded[out++] = (char)c;
        }
    }
    decoded[out] = '\0';
    return decoded;
}

static cwist_http_response *file_read_response(file_job *job) {
    int fd = open(job->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > CWIST_HTTP_MAX_BODY_SIZE) {
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    cwist_http_response *response = cwist_http_response_create();
    if (!response) {
        close(fd);
        return NULL;
    }
    response->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(response->status_text, "OK");
    cwist_http_header_add(&response->headers, "Content-Type", "application/octet-stream");

    if (job->method != CWIST_HTTP_HEAD && len > 0) {
        char *data = cwist_alloc(len);
        if (!data) {
            close(fd);
            cwist_http_response_destroy(response);
            return NULL;
        }
        size_t done = 0;
        while (done < len) {
            ssize_t n = read(fd, data + done, len - done);
            if (n > 0) {
                done += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct timespec pause = {.tv_nsec = 1000000L};
                nanosleep(&pause, NULL);
                continue;
            }
            break;
        }
        if (done != len) {
            cwist_free(data);
            close(fd);
            cwist_http_response_destroy(response);
            return NULL;
        }
        cwist_error_t err = cwist_sstring_assign_len(response->body, data, len);
        bool ok = cwist_error_is_ok(&err);
        cwist_error_dispose(&err);
        cwist_free(data);
        if (!ok) {
            close(fd);
            cwist_http_response_destroy(response);
            return NULL;
        }
    }
    close(fd);
    cwist_http_async_disown_response(response);
    return response;
}

static void file_job_perform(void *arg) {
    file_job *job = arg;
    cwist_http_response *response = file_read_response(job);
    pthread_mutex_lock(&job->lock);
    if (job->detached) {
        if (response) cwist_http_response_destroy(response);
    } else {
        job->response = response;
    }
    job->done = true;
    pthread_cond_broadcast(&job->cond);
    pthread_mutex_unlock(&job->lock);
    cwist_io_queue_stop(job->queue);
}

static void *file_worker_main(void *arg) {
    file_job *job = arg;
    cwist_io_queue_run(job->queue);
    cwist_io_queue_destroy(job->queue);
    pthread_mutex_lock(&job->lock);
    bool detached = job->detached;
    pthread_mutex_unlock(&job->lock);
    if (detached) {
        pthread_cond_destroy(&job->cond);
        pthread_mutex_destroy(&job->lock);
        cwist_free(job->path);
        cwist_free(job);
    }
    return NULL;
}

int cwist_http_client_file_request(const char *url, cwist_http_method_t method, int timeout_ms,
                                   cwist_http_response **out_response) {
    if (!url || !out_response || (method != CWIST_HTTP_GET && method != CWIST_HTTP_HEAD)) return -1;
    *out_response = NULL;
    char *path = file_url_path(url);
    if (!path) return -1;

    file_job *job = cwist_alloc(sizeof(*job));
    if (!job) {
        cwist_free(path);
        return -1;
    }
    if (cwist_full_gc_enabled()) {
        cwist_gc_scope_disown(path);
        cwist_gc_scope_disown(job);
    }
    job->path = path;
    job->method = method;
    job->queue = cwist_io_queue_create(1);
    bool lock_ready = pthread_mutex_init(&job->lock, NULL) == 0;
    bool cond_ready = lock_ready && file_cond_init(&job->cond) == 0;
    if (!job->queue || !lock_ready || !cond_ready) {
        if (job->queue) cwist_io_queue_destroy(job->queue);
        if (cond_ready) pthread_cond_destroy(&job->cond);
        if (lock_ready) pthread_mutex_destroy(&job->lock);
        cwist_free(path);
        cwist_free(job);
        return -1;
    }
    if (!cwist_io_queue_submit(job->queue, file_job_perform, job)) {
        cwist_io_queue_destroy(job->queue);
        pthread_cond_destroy(&job->cond);
        pthread_mutex_destroy(&job->lock);
        cwist_free(path);
        cwist_free(job);
        return -1;
    }

    pthread_t worker;
    if (pthread_create(&worker, NULL, file_worker_main, job) != 0) {
        cwist_io_queue_stop(job->queue);
        cwist_io_queue_destroy(job->queue);
        pthread_cond_destroy(&job->cond);
        pthread_mutex_destroy(&job->lock);
        cwist_free(path);
        cwist_free(job);
        return -1;
    }
    pthread_mutex_lock(&job->lock);
    if (timeout_ms > 0) {
        struct timespec deadline;
        file_deadline(&deadline, timeout_ms);
        while (!job->done) {
            int rc = pthread_cond_timedwait(&job->cond, &job->lock, &deadline);
            if (rc == ETIMEDOUT) {
                job->detached = true;
                break;
            }
        }
    } else {
        while (!job->done) pthread_cond_wait(&job->cond, &job->lock);
    }
    bool completed = job->done && !job->detached;
    pthread_mutex_unlock(&job->lock);
    if (!completed) {
        pthread_detach(worker);
        return -1;
    }

    pthread_join(worker, NULL);
    *out_response = job->response;
    pthread_cond_destroy(&job->cond);
    pthread_mutex_destroy(&job->lock);
    cwist_free(path);
    cwist_free(job);
    return *out_response ? 0 : -1;
}
