#define _POSIX_C_SOURCE 200809L

#include <cwist/https.h>
#include <cwist/sstring.h>
#include <cwist/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

/* --- Internal Error Helpers --- */

/* Monotonic clock in milliseconds, for connection deadlines. */
static uint64_t cwist_https_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Poll the socket for the direction OpenSSL is waiting on.
 * @return 0 if the requested event is ready, -1 on timeout/error.
 */
static int cwist_ssl_wait(int fd, int ssl_error, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = 0 };
    if (ssl_error == SSL_ERROR_WANT_READ) {
        pfd.events = POLLIN;
    } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
        pfd.events = POLLOUT;
    } else {
        return -1;
    }

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 0;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
};

/* --- Thread Pool for HTTPS --- */
#define HTTPS_TASK_QUEUE_SIZE 2097152

typedef struct {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
} https_pool_task_t;

typedef struct {
    pthread_t threads[2048];
    size_t    threads_size;
    https_pool_task_t queue[HTTPS_TASK_QUEUE_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int shutdown;
} https_thread_pool_t;

static https_thread_pool_t g_https_pool;
static bool g_https_pool_initialized = false;

// Forward declaration of existing https_thread_handler
static void *https_thread_handler(void *arg);

static void *https_pool_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_https_pool.mutex);
        while (g_https_pool.count == 0 && !g_https_pool.shutdown) {
            pthread_cond_wait(&g_https_pool.cond_not_empty, &g_https_pool.mutex);
        }
        if (g_https_pool.shutdown) {
            pthread_mutex_unlock(&g_https_pool.mutex);
            break;
        }
        https_pool_task_t task = g_https_pool.queue[g_https_pool.head];
        g_https_pool.head = (g_https_pool.head + 1) % HTTPS_TASK_QUEUE_SIZE;
        g_https_pool.count--;
        pthread_cond_signal(&g_https_pool.cond_not_full);
        pthread_mutex_unlock(&g_https_pool.mutex);

        // We can reuse the existing https_thread_handler logic by wrapping the task
        struct https_thread_payload *payload = malloc(sizeof(*payload));
        if (payload) {
            payload->client_fd = task.client_fd;
            payload->ctx = task.ctx;
            payload->handler = task.handler;
            payload->user_ctx = task.user_ctx;
            https_thread_handler(payload);
        } else {
            close(task.client_fd);
        }
    }
    return NULL;
}

int https_pool_init(void) {
    if (g_https_pool_initialized) return 0;
    memset(&g_https_pool, 0, sizeof(g_https_pool));
    pthread_mutex_init(&g_https_pool.mutex, NULL);
    pthread_cond_init(&g_https_pool.cond_not_empty, NULL);
    pthread_cond_init(&g_https_pool.cond_not_full, NULL);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        if (pthread_create(&g_https_pool.threads[i], NULL, https_pool_worker, NULL) != 0) {
            return -1;
        }
    }
    g_https_pool_initialized = true;
    return 0;
}

void https_pool_submit(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    pthread_mutex_lock(&g_https_pool.mutex);
    while (g_https_pool.count >= HTTPS_TASK_QUEUE_SIZE && !g_https_pool.shutdown) {
        pthread_cond_wait(&g_https_pool.cond_not_full, &g_https_pool.mutex);
    }
    if (g_https_pool.shutdown) {
        pthread_mutex_unlock(&g_https_pool.mutex);
        close(client_fd);
        return;
    }
    g_https_pool.queue[g_https_pool.tail].client_fd = client_fd;
    g_https_pool.queue[g_https_pool.tail].ctx = ctx;
    g_https_pool.queue[g_https_pool.tail].handler = handler;
    g_https_pool.queue[g_https_pool.tail].user_ctx = user_ctx;
    g_https_pool.tail = (g_https_pool.tail + 1) % HTTPS_TASK_QUEUE_SIZE;
    g_https_pool.count++;
    pthread_cond_signal(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
}

void https_pool_destroy(void) {
    if (!g_https_pool_initialized) return;
    pthread_mutex_lock(&g_https_pool.mutex);
    g_https_pool.shutdown = 1;
    pthread_cond_broadcast(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        pthread_join(g_https_pool.threads[i], NULL);
    }
    pthread_mutex_destroy(&g_https_pool.mutex);
    pthread_cond_destroy(&g_https_pool.cond_not_empty);
    pthread_cond_destroy(&g_https_pool.cond_not_full);

    g_https_pool_initialized = false;
}
/* --- End Thread Pool --- */

#define CWIST_ALPN_HTTP11       ((const unsigned char *)"\x08http/1.1")
#define CWIST_ALPN_H2_HTTP11    ((const unsigned char *)"\x02h2\x08http/1.1")
#define CWIST_ALPN_H3_H2_HTTP11 ((const unsigned char *)"\x02h3\x02h2\x08http/1.1")
#define CWIST_ALPN_HTTP11_LEN       9
#define CWIST_ALPN_H2_HTTP11_LEN    12
#define CWIST_ALPN_H3_H2_HTTP11_LEN 15

/**
 * @file https.c
 * @brief OpenSSL-backed HTTPS accept, receive, send, and server-loop helpers.
 */

/**
 * @brief Build a JSON-rich cwist_error_t from the latest OpenSSL error state.
 * @param msg Human-readable message describing the failing HTTPS step.
 * @return Error object with module, message, and OpenSSL error string fields.
 */
static cwist_error_t make_ssl_error(const char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    
    unsigned long ssl_err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(ssl_err, buf, sizeof(buf));
    
    cJSON_AddStringToObject(err.error.err_json, "module", "https");
    cJSON_AddStringToObject(err.error.err_json, "message", msg);
    cJSON_AddStringToObject(err.error.err_json, "openssl_error", buf);
    
    return err;
}

/* --- Context Management --- */

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    // Initialize OpenSSL
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return make_ssl_error("Unable to create SSL context");
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load private key");
    }

    // Verify key matches cert
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Private key does not match certificate");
    }

    *ctx = (cwist_https_context*)malloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;

    err.error.err_i16 = 0; // Success
    return err;
}

void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        free(ctx);
        EVP_cleanup();
    }
}

/* --- Connection Handling --- */

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !ctx->ctx || client_fd < 0) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        return make_ssl_error("Failed to create SSL structure");
    }

    SSL_set_fd(ssl, client_fd);

    /* Bound the whole handshake so a client dribbling bytes cannot pin a
     * pool worker forever. */
    uint64_t handshake_deadline = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;
    int rc;
    while ((rc = SSL_accept(ssl)) <= 0) {
        int ssl_err = SSL_get_error(ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            uint64_t now = cwist_https_now_ms();
            if (now >= handshake_deadline) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out");
                SSL_free(ssl);
                return err_obj;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t remaining = handshake_deadline - now;
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;
            if (cwist_ssl_wait(client_fd, ssl_err, wait_ms) != 0) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out or socket error");
                SSL_free(ssl);
                return err_obj;
            }
            continue;
        }
        cwist_error_t err_obj = make_ssl_error("SSL handshake failed");
        SSL_free(ssl);
        return ssl_err;
    }

    *conn = (cwist_https_connection*)malloc(sizeof(cwist_https_connection));
    if (!*conn) {
        SSL_free(ssl);
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = malloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        SSL_free(ssl);
        free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';

    err.error.err_i16 = 0;
    return err;
}

void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            close(conn->fd);
        }
        free(conn->read_buf);
        free(conn);
    }
}

/* --- I/O Operations --- */

cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl || !conn->read_buf) return NULL;

    size_t total_received = conn->buf_len;
    char *header_end = NULL;

    /* Total deadline for assembling the request headers. */
    uint64_t headers_deadline = cwist_https_now_ms() + CWIST_HTTP_HEADERS_TIMEOUT_MS;

    while (!(header_end = strstr(conn->read_buf, "\r\n\r\n"))) {
        if (total_received >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            return NULL;
        }

        uint64_t now = cwist_https_now_ms();
        if (now >= headers_deadline) {
            return NULL;
        }
        int wait_ms = CWIST_HTTP_TIMEOUT_MS;
        uint64_t remaining = headers_deadline - now;
        if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;

        struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
        int pret = poll(&pfd, 1, wait_ms);
        if (pret <= 0) {
            return NULL;
        }

        int bytes = SSL_read(conn->ssl, conn->read_buf + total_received, (int)(CWIST_HTTP_READ_BUFFER_SIZE - 1 - total_received));
        if (bytes <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, bytes);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return NULL;
        }

        total_received += (size_t)bytes;
        conn->read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(conn->read_buf);
    if (!req) return NULL;

    req->client_fd = conn->fd;

    size_t header_len = (header_end + 4) - conn->read_buf;
    size_t body_received = total_received - header_len;

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        char *body = malloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = body_received < req->content_length ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        /* No total cap (slow 1 GiB uploads must keep working); abort only
         * when no bytes arrive for a cumulative idle span.  Any successful
         * read resets the idle clock. */
        uint64_t body_idle_start = cwist_https_now_ms();

        while (current_body_len < req->content_length) {
            uint64_t now = cwist_https_now_ms();
            if (now - body_idle_start >= CWIST_HTTP_BODY_IDLE_TIMEOUT_MS) {
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t idle_left = CWIST_HTTP_BODY_IDLE_TIMEOUT_MS - (now - body_idle_start);
            if (idle_left < (uint64_t)wait_ms) wait_ms = (int)idle_left;

            struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
            int pret = poll(&pfd, 1, wait_ms);
            if (pret <= 0) {
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len, (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
            body_idle_start = cwist_https_now_ms();
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        free(body);

        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(conn->read_buf, header_end + 4 + req->content_length, leftover_len);
            conn->buf_len = leftover_len;
        } else {
            conn->buf_len = 0;
        }
    } else {
        if (body_received > 0) {
            memmove(conn->read_buf, header_end + 4, body_received);
            conn->buf_len = body_received;
        } else {
            conn->buf_len = 0;
        }
    }
    conn->read_buf[conn->buf_len] = '\0';

    return req;
}

cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 1. Serialize using existing HTTP logic
    cwist_sstring *response_str = cwist_http_stringify_response(res);
    if (!response_str) {
        err.error.err_i16 = -1;
        return err;
    }

    // 2. Send over SSL
    const char *p = response_str->data;
    int left = (int)response_str->size;
    int total_sent = 0;

    err.error.err_i16 = 0; // Assume success initially

    while (left > 0) {
        int sent = SSL_write(conn->ssl, p, left);
        if (sent <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, sent);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                continue; // Retry
            }
            err = make_ssl_error("SSL write failed");
            break;
        }
        p += sent;
        left -= sent;
        total_sent += sent;
    }

    cwist_sstring_destroy(response_str);
    return err;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
};

static void *https_thread_handler(void *arg) {
    struct https_thread_payload *payload = (struct https_thread_payload *)arg;
    cwist_https_connection *conn = NULL;
    cwist_error_t hs_err = cwist_https_accept(payload->ctx, payload->client_fd, &conn);
    
    if (hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0) {
        payload->handler(conn, payload->user_ctx);
        cwist_https_close_connection(conn);
    } else {
        if (hs_err.errtype == CWIST_ERR_JSON) {
            cJSON_Delete(hs_err.error.err_json);
        }
        close(payload->client_fd);
    }
    
    free(payload);
    return NULL;
}

cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue; 
        }

        /* Reap vanished peers within ~2 minutes instead of the ~2h kernel
         * default, so dead connections cannot park pool workers forever. */
        {
            int one = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
            int keepidle = 60;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
            int keepintvl = 10;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
            int keepcnt = 6;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
        }

        https_pool_submit(client_fd, ctx, handler, user_ctx);
    }
    
    return err;
}
