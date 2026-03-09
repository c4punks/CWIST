#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

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

/**
 * @brief Initialize OpenSSL and create a server TLS context from PEM files.
 * @param ctx Output pointer that receives the allocated HTTPS context.
 * @param cert_path Path to the PEM certificate chain.
 * @param key_path Path to the PEM private key.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    bool enable_http3 = options && options->enable_http3;
    bool enable_http2 = options && (options->enable_http2 || enable_http3);

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

    if (SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to enforce TLS minimum version");
    }
    cwist_https_apply_base_tls_defaults(ssl_ctx);

    if (!cwist_tls_apply_pqc_layer(app, ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("PQC layer configuration failed");
    }

    if (enable_http2) {
        err = cwist_https_apply_http2_tls_profile(ssl_ctx);
        if (!cwist_error_is_ok(&err)) {
            SSL_CTX_free(ssl_ctx);
            return err;
        }
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to complete certificate chain");
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

    *ctx = (cwist_https_context *)cwist_alloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;
    (*ctx)->http2_enabled = enable_http2;
    (*ctx)->http3_enabled = enable_http3;
    (*ctx)->ticket_key = NULL;

    /* Pre-fork shared ticket key so every forked worker can resume tickets
     * issued by any other worker. Must run before workers are forked. */
    cwist_tls_setup_shared_ticket_key(ssl_ctx, &(*ctx)->ticket_key);

    SSL_CTX_set_alpn_select_cb(ssl_ctx, cwist_https_alpn_select_cb, (void *)*ctx);

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Free an HTTPS context and release its OpenSSL resources.
 * @param ctx Context to destroy.
 */
void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        if (ctx->ticket_key) {
            /* Scrub key material before releasing it. */
            OPENSSL_cleanse(ctx->ticket_key, sizeof(cwist_tls_ticket_key));
            cwist_free(ctx->ticket_key);
        }
        cwist_free(ctx);
        /* EVP_cleanup() intentionally not called: it is deprecated since
         * OpenSSL 1.1.0 and tears down global state other code may use. */
    }
}

/**
 * @brief Wrap an accepted TCP client socket in an OpenSSL connection object.
 * @param ctx HTTPS context holding the configured SSL_CTX.
 * @param client_fd Accepted TCP socket descriptor.
 * @param conn Output pointer that receives the allocated connection wrapper.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    *conn = (cwist_https_connection *)cwist_alloc(sizeof(cwist_https_connection));
    if (!*conn) {
        err.error.err_i16 = -1;
        return err;
    }

    memset(*conn, 0, sizeof(**conn));
    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        cwist_free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';
    (*conn)->negotiated_http2 = false;
    (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP11;
    (*conn)->http3_enabled = ctx->http3_enabled;
    (*conn)->deferred = false;

    if (cwist_full_gc_enabled()) {
        /* *conn and its read_buf were just handed to the connection
         * registry's close_fn (below) as their sole eventual releaser.
         * They must NOT also sit on this thread's generic cwist_alloc
         * pending-sweep list: that list's own thread-exit destructor runs
         * independently of (and in unspecified order relative to) the
         * connection registry's destructor, so if it fired first it would
         * cwist_ebr_free() *conn out from under the registry's still-
         * pending entry -- the registry's sweep would then read a freed
         * conn->ssl. Disowning here hands sole ownership to the registry,
         * exactly the cross-list handoff cwist_gc_scope_disown() exists
         * for (see io_queue.c's job-handoff use of the same call). */
        cwist_gc_scope_disown(*conn);
        cwist_gc_scope_disown((*conn)->read_buf);
        /* Full-GC's safety net: if this connection is never explicitly
         * closed (worker crashes mid-request, an error path forgets), the
         * owning thread's exit -- or, failing that, process exit -- still
         * closes it instead of leaking the fd/TLS session. Normal explicit
         * close (cwist_https_close_connection) untracks first, so this is
         * a no-op on the common path. Registering here (rather than only
         * in cwist_https_accept) covers the shepherd's pre-handshaked
         * dispatch path too, which calls this function directly. */
        cwist_conn_registry_track(*conn, https_close_conn_cb);
    }

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        (*conn)->negotiated_http2 = true;
        (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2;
    }
    /* h3 is QUIC-only and never negotiated over TCP TLS */

    /* The handshake's final client flight was just ACKed; stay in quickack
     * mode so the request that follows (held back by a Nagle-bound client)
     * is ACKed immediately and the connection does not stall one
     * delayed-ACK window before the request phase even starts. */
    cwist_tcp_quickack(client_fd);

    /* Both handshake paths funnel through here, so this is the single
     * choke point for TLS observability (issue #306). */
    https_tls_record_handshake(ssl);
    atomic_fetch_add_explicit(&g_tls_connections_active, 1, memory_order_relaxed);

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief cwist_conn_registry_track() callback adapter: the registry only
 *        knows `void (*)(void *)`. Points at the raw teardown
 *        (https_connection_teardown), NOT the public
 *        cwist_https_close_connection() -- see that raw helper's doc
 *        comment for why calling back into the registry from here would
 *        be a bug, not just redundant.
 */
static void https_close_conn_cb(void *handle) {
    https_connection_teardown((cwist_https_connection *)handle);
}

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd,
                                 cwist_https_connection **conn) {
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
    cwist_tcp_quickack(client_fd);

    /* Bound the whole handshake so a client dribbling bytes cannot pin a
     * pool worker forever. */
    uint64_t handshake_deadline = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;
    int rc;
    while ((rc = SSL_accept(ssl)) <= 0) {
        /* Keep immediate ACKs armed: the kernel drops out of quickack mode
         * as the handshake segments flow, and the delayed ACK of the
         * client's Finished would hold a Nagle-bound request by ~40 ms. */
        cwist_tcp_quickack(client_fd);
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
        return err_obj;
    }

    cwist_error_t wrap_err = https_wrap_established(ctx, client_fd, ssl, conn);
    if (!cwist_error_is_ok(&wrap_err)) {
        SSL_free(ssl);
        return wrap_err;
    }
    return wrap_err;
}

/**
 * @brief Gracefully close an HTTPS connection and free its buffers.
 * @param conn HTTPS connection wrapper to close.
 */
void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        /* Balances the increment at wrap time; teardown is the single exit
         * point for every established connection (public close and the
         * full-GC registry sweep alike). */
        atomic_fetch_sub_explicit(&g_tls_connections_active, 1, memory_order_relaxed);
        if (conn->proto_state && conn->proto_state_free) {
            conn->proto_state_free(conn->proto_state);
            conn->proto_state = NULL;
        }
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            /* close() on a socket with unread receive-queue data makes the
             * kernel answer with RST instead of a graceful FIN. TLS clients
             * routinely send their close_notify (or a pipelined next request)
             * that the request path never consumed, so under connection churn
             * nearly every teardown became an abortive close: the peer's
             * kernel flushes queued response bytes on the RST (the "read
             * error" burst load clients report) and the socket never settles
             * through TIME_WAIT normally. Drain whatever is pending before
             * closing; the socket is about to be destroyed anyway. */
            char drain[2048];
            ssize_t n;
            int guard = 16;
            while (guard-- > 0 && (n = recv(conn->fd, drain, sizeof(drain), MSG_DONTWAIT)) > 0) {
                /* discard */
            }
            (void)n;
            close(conn->fd);
        }
        cwist_free(conn->read_buf);
        cwist_free(conn);
    }
}

/**
 * @brief Read from the TLS stream until a full HTTP request has been assembled.
 * @param conn Active HTTPS connection wrapper.
 * @return Parsed HTTP request, or NULL on timeout, parse failure, or IO failure.
 */
cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl || !conn->read_buf) return NULL;

    size_t total_received = conn->buf_len;
    char *header_end = NULL;

    /* Total deadline for assembling the request headers. */
    uint64_t headers_deadline = cwist_https_now_ms() + CWIST_HTTP_HEADERS_TIMEOUT_MS;

    /* Rescanning from offset 0 on every read is O(n^2) over dribbled
     * headers; resume 3 bytes back so a terminator split across two reads
     * is still found. */
    size_t scan_from = 0;
    bool deadline_grace_used = false;

    while (1) {
        header_end = strstr(conn->read_buf + scan_from, "\r\n\r\n");
        if (header_end) break;
        scan_from = total_received > 3 ? total_received - 3 : 0;

        if (total_received >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            return NULL;
        }

        if (SSL_pending(conn->ssl) == 0) {
            uint64_t now = cwist_https_now_ms();
            if (now >= headers_deadline) {
                /* A request may have landed just as the idle deadline
                 * expired; dropping it here makes strict clients surface a
                 * network protocol error where lenient ones silently retry.
                 * Give the wire one non-blocking chance before closing.
                 * Bounded to a single retry so a dribbling peer cannot pin
                 * the worker past the deadline. */
                bool readable = false;
                if (!deadline_grace_used) {
                    deadline_grace_used = true;
                    struct pollfd gfd = {.fd = conn->fd, .events = POLLIN};
                    readable = (poll(&gfd, 1, 0) > 0 && (gfd.revents & POLLIN));
                }
                if (!readable) {
                    return NULL;
                }
            } else {
                int wait_ms = CWIST_HTTP_TIMEOUT_MS;
                uint64_t remaining = headers_deadline - now;
                if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;

                struct pollfd pfd = {.fd = conn->fd, .events = POLLIN};
                int pret = poll(&pfd, 1, wait_ms);
                if (pret <= 0) {
                    return NULL;
                }
            }
        }

        int bytes = SSL_read(conn->ssl, conn->read_buf + total_received,
                             (int)(CWIST_HTTP_READ_BUFFER_SIZE - 1 - total_received));
        if (bytes <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, bytes);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) return NULL;
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
    req->https_conn = conn;

    size_t header_len = (header_end + 4) - conn->read_buf;
    size_t body_received = total_received - header_len;

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        char *body = cwist_alloc(req->content_length + 1);
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
            if (SSL_pending(conn->ssl) == 0) {
                uint64_t now = cwist_https_now_ms();
                if (now - body_idle_start >= CWIST_HTTP_BODY_IDLE_TIMEOUT_MS) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
                int wait_ms = CWIST_HTTP_TIMEOUT_MS;
                uint64_t idle_left = CWIST_HTTP_BODY_IDLE_TIMEOUT_MS - (now - body_idle_start);
                if (idle_left < (uint64_t)wait_ms) wait_ms = (int)idle_left;

                struct pollfd pfd = {.fd = conn->fd, .events = POLLIN};
                int pret = poll(&pfd, 1, wait_ms);
                if (pret <= 0) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len,
                                 (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) {
                        cwist_free(body);
                        cwist_http_request_destroy(req);
                        return NULL;
                    }
                    continue;
                }
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
            body_idle_start = cwist_https_now_ms();
        }
        body[req->content_length] = '\0';
        /* Adopt the filled buffer: one allocation, zero copies. */
        cwist_sstring_adopt_len(req->body, body, req->content_length);

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

/**
 * @brief Serialize an HTTP response and send it over an active TLS connection.
 * @param conn Active HTTPS connection wrapper.
 * @param res Response object to serialize.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // Inject Alt-Svc when HTTP/3 is enabled so clients discover the QUIC endpoint,
    // unless the application already set or cleared the Alt-Svc header.
    if (conn->http3_enabled && !cwist_http_header_get(res->headers, "Alt-Svc")) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int port = 443;
        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
    }

    // 1+2. Headers onto a stack buffer; a small body rides in the same TLS
    // record, saving one record's AEAD tag and one syscall on the common
    // short-response path. Larger bodies keep the split writes (BoringSSL
    // records a big SSL_write at 16 KiB internally).
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));

    const char *body_ptr = NULL;
    size_t body_len = 0;
    if (res->is_ptr_body) {
        body_ptr = (const char *)res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body_ptr = res->body->data;
        body_len = res->body->size;
    }

    bool body_coalesced = false;
    if (body_ptr && body_len > 0 && body_len <= CWIST_TLS_COALESCE_MAX) {
        char *combined = (char *)cwist_alloc(header_len + body_len);
        if (combined) {
            memcpy(combined, header_buf, header_len);
            memcpy(combined + header_len, body_ptr, body_len);
            int rc = cwist_ssl_write_all(conn, combined, header_len + body_len);
            cwist_free(combined);
            if (rc != 0) {
                return make_ssl_error("SSL coalesced write failed");
            }
            body_coalesced = true;
        }
        /* Allocation failure: fall through to the split writes. */
    }
    if (!body_coalesced) {
        if (cwist_ssl_write_all(conn, header_buf, header_len) != 0) {
            return make_ssl_error("SSL header write failed");
        }
        if (body_ptr && body_len > 0) {
            if (cwist_ssl_write_all(conn, body_ptr, body_len) != 0) {
                return make_ssl_error("SSL body write failed");
            }
        }
    }

    // 3. File streams cannot use sendfile() through userland TLS; chunk them
    if (res->use_file_stream && res->file_stream_fd >= 0) {
        char fbuf[65536];
        size_t remaining = res->file_stream_len;
        off_t offset = res->file_stream_offset;
        while (remaining > 0) {
            size_t to_read = remaining < sizeof(fbuf) ? remaining : sizeof(fbuf);
            ssize_t n = pread(res->file_stream_fd, fbuf, to_read, offset);
            if (n < 0) {
                if (errno == EINTR) continue;
                return make_ssl_error("file stream read failed");
            }
            if (n == 0) break;
            if (cwist_ssl_write_all(conn, fbuf, (size_t)n) != 0) {
                return make_ssl_error("SSL file stream write failed");
            }
            offset += n;
            remaining -= (size_t)n;
        }
        res->file_stream_offset = offset;
    }

    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_https_send_response_head(cwist_https_connection *conn,
                                             cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    if (conn->http3_enabled && !cwist_http_header_get(res->headers, "Alt-Svc")) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int port = 443;
        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
    }

    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));
    if (cwist_ssl_write_all(conn, header_buf, header_len) != 0) {
        return make_ssl_error("SSL header write failed");
    }

    err.error.err_i16 = 0;
    return err;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
};

/**
 * @brief Worker entry point that performs the TLS handshake before dispatching.
 * @param arg Thread payload containing the accepted socket and dispatch callback.
 * @return Always NULL for pthread compatibility.
 */
static void *https_thread_handler(void *arg) {
    struct https_thread_payload *payload = (struct https_thread_payload *)arg;
    cwist_https_connection *conn = NULL;
    cwist_error_t hs_err;

    if (payload->conn) {
        conn = payload->conn;
        hs_err = make_error(CWIST_ERR_INT16);
        hs_err.error.err_i16 = 0;
    } else if (payload->pres_ssl) {
        /* Handshake already completed by the shepherd thread. */
        hs_err = https_wrap_established(payload->ctx, payload->client_fd, payload->pres_ssl, &conn);
        if (!cwist_error_is_ok(&hs_err)) {
            SSL_free(payload->pres_ssl);
        }
    } else {
        hs_err = cwist_https_accept(payload->ctx, payload->client_fd, &conn);
    }

    if (cwist_error_is_ok(&hs_err)) {
        conn->pool_ctx = payload->ctx;
        conn->pool_handler = payload->handler;
        conn->pool_user_ctx = payload->user_ctx;
        t_https_parked = false;
        payload->handler(conn, payload->user_ctx);
        if (t_https_parked) {
            t_https_parked = false;
        } else if (!conn->deferred) {
            cwist_https_close_connection(conn);
        }
    } else {
        if (hs_err.errtype == CWIST_ERR_JSON) {
            cJSON_Delete(hs_err.error.err_json);
        }
        if (payload->conn) {
            cwist_https_close_connection(payload->conn);
        } else {
            close(payload->client_fd);
        }
    }
    
    free(payload);
    return NULL;
}

/**
 * @brief Accept HTTPS clients in a loop and dispatch each one to the supplied handler.
 * @param server_fd Bound listening socket descriptor.
 * @param ctx HTTPS context shared by all accepted connections.
 * @param handler Callback invoked for each successful TLS client wrapper.
 * @param user_ctx Opaque pointer forwarded to the handler.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    if (https_pool_init() != 0) {
        err.error.err_i16 = -1;
        return err;
    }

    while (atomic_load(&g_cwist_running)) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue; 
        }

        pthread_t thread;
        struct https_thread_payload *payload = malloc(sizeof(*payload));
        if (!payload) {
            close(client_fd);
            continue;
        }

        if (pthread_create(&thread, NULL, https_thread_handler, payload) == 0) {
            pthread_detach(thread);
        } else {
            free(payload);
            close(client_fd);
        }

        cwist_https_dispatch(client_fd, ctx, handler, user_ctx);
    }

    https_pool_destroy();
    return err;
}
