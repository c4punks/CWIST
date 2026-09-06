/**
 * @file http.h
 * @brief HTTP Protocol Definitions and Helpers.
 */

#ifndef __CWIST_HTTP_H__
#define __CWIST_HTTP_H__

#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/net/http/query.h>
#include <cwist/core/db/sql.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

long get_cpu_cores(void);
long get_optimal_thread_count(void);

/** --- Enums --- */

typedef enum cwist_http_method_t {
    CWIST_HTTP_GET,
    CWIST_HTTP_POST,
    CWIST_HTTP_PUT,
    CWIST_HTTP_DELETE,
    CWIST_HTTP_PATCH,
    CWIST_HTTP_HEAD,
    CWIST_HTTP_OPTIONS,
    CWIST_HTTP_UNKNOWN
} cwist_http_method_t;

typedef enum cwist_http_status_t {
    CWIST_HTTP_OK = 200,
    CWIST_HTTP_CREATED = 201,
    CWIST_HTTP_NO_CONTENT = 204,
    CWIST_HTTP_NOT_MODIFIED = 304,
    CWIST_HTTP_BAD_REQUEST = 400,
    CWIST_HTTP_UNAUTHORIZED = 401,
    CWIST_HTTP_FORBIDDEN = 403,
    CWIST_HTTP_NOT_FOUND = 404,
    CWIST_HTTP_RANGE_NOT_SATISFIABLE = 416,
    CWIST_HTTP_INTERNAL_ERROR = 500,
    CWIST_HTTP_NOT_IMPLEMENTED = 501,
    CWIST_HTTP_SERVICE_UNAVAILABLE = 503
} cwist_http_status_t;

/** @brief Failure reason reported by the request receive APIs.
 * Distinguishes protocol errors (which deserve an error response before
 * close) from an orderly client disconnect (close quietly). */
typedef enum cwist_http_parse_error_t {
    CWIST_HTTP_PARSE_OK = 0,          /* No error. */
    CWIST_HTTP_PARSE_EOF,             /* Orderly close / no data: close quietly. */
    CWIST_HTTP_PARSE_MALFORMED,       /* 400: bad request-line, Host rules, CL/TE rules. */
    CWIST_HTTP_PARSE_HEADER_OVERFLOW, /* 431: header block exceeds the read buffer. */
    CWIST_HTTP_PARSE_BODY_TOO_LARGE,  /* 413: Content-Length exceeds the body cap. */
    CWIST_HTTP_PARSE_TE_UNSUPPORTED,  /* 501: unsupported transfer coding. */
    CWIST_HTTP_PARSE_EXPECT_FAILED    /* 417: unsupported Expect value. */
} cwist_http_parse_error_t;

/** --- Constants and Limits --- */
#define CWIST_HTTP_MAX_HEADER_SIZE (8 * 1024)
#define CWIST_HTTP_MAX_BODY_SIZE (10 * 1024 * 1024)
#define CWIST_HTTP_READ_BUFFER_SIZE (16 * 1024)
#define CWIST_HTTP_TIMEOUT_MS      30000
#define CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS 45000  /* Total TLS handshake budget */
#define CWIST_HTTP_HEADERS_TIMEOUT_MS    120000 /* Total header read budget */
#define CWIST_HTTP_BODY_IDLE_TIMEOUT_MS  60000  /* Abort body read after this much silence */
#define CWIST_HTTP2_IDLE_TIMEOUT_MS      300000 /* Default h2 idle budget (env overridable) */

/** --- Structures --- */

/// Linked list for headers to handle multiple headers easily
typedef struct cwist_http_header_node {
    cwist_sstring *key;
    cwist_sstring *value;
    struct cwist_http_header_node *next;
    bool arena_owned;   ///< True when the node lives in a request arena (freed with the arena).
} cwist_http_header_node;

typedef struct cwist_http_request {
    cwist_http_method_t method;
    cwist_sstring *path;        ///< e.g., "/users/1"
    cwist_sstring *query;       ///< e.g., "active=true" (raw)
    cwist_query_map *query_params; ///< Parsed query parameters
    cwist_query_map *path_params;  ///< Parsed path parameters (e.g. :id)
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    bool keep_alive;
    int client_fd;
    bool upgraded;
    uint32_t stream_id;     ///< HTTP/2 or HTTP/3 stream ID (0 for HTTP/1.1).
    void *private_data;     ///< Internal framework use (protocol-specific context).
    void *route_middleware_state; ///< Router middleware chain state (internal).
    size_t content_length;
    cwist_endpoint_opt_t endpoint_opts; ///< Behavior hints for the active endpoint.
    cwist_query_map *flash; ///< Flash messages for this request (one-time read).
    void *session;          ///< cwist_session_t pointer set by session middleware.
    char *csrf_token;       ///< CSRF token populated by csrf middleware.
    void *arena;            ///< Per-request arena for bump-allocated structs (internal).
    void *async_conn;       ///< C1M connection shell when on the reactor path (internal).
    bool ws_async_handoff;  ///< WebSocket async path took fd ownership; C1M layer must detach.
    void *https_conn;       ///< HTTPS connection wrapper when on TLS path (internal).
    void *h2_queue;         ///< HTTP/2 async queue when on H2 path (internal).
} cwist_http_request;

typedef void (*cwist_http_body_cleanup_fn)(const void *ptr, size_t len, void *ctx);

/**
 * @brief HTTP Response Object.
 * Supports standard string body or Zero-Copy pointer body.
 */
typedef struct cwist_http_response {
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_status_t status_code;
    cwist_sstring *status_text; ///< e.g., "OK"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    cwist_endpoint_opt_t endpoint_opts; ///< Mirrors req->endpoint_opts.
    
    // Zero-Copy Pointer Body
    bool is_ptr_body;        ///< If true, body data is read from ptr_body
    const void *ptr_body;    ///< Pointer to external data (e.g., mmap region)
    size_t ptr_body_len;     ///< Length of external data
    cwist_http_body_cleanup_fn ptr_body_cleanup; ///< Optional release hook
    void *ptr_body_cleanup_ctx; ///< User data for release hook

    /// Fast File Streaming
    bool use_file_stream;        ///< True when sendfile/splice path is used.
    int file_stream_fd;          ///< Open descriptor for sendfile.
    size_t file_stream_len;      ///< Total bytes to stream.
    off_t file_stream_offset;    ///< Current offset for sendfile loop.
    bool file_stream_auto_close; ///< Close fd after streaming.
    
    bool keep_alive;
    
    /// Alt-Svc header for HTTP/3 upgrade advertisement
    char *alt_svc;

    void *arena;            ///< Per-response arena for bump-allocated structs (internal).
} cwist_http_response;

/** --- API Functions --- */

/** @name Request Lifecycle */
/** @{ */
cwist_http_request *cwist_http_request_create(void);
void cwist_http_request_destroy(cwist_http_request *req);
cwist_http_request *cwist_http_parse_request(const char *raw_request); 
cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len, cwist_http_parse_error_t *err_out);
/**
 * @brief Send a minimal HTTP/1.x error response (Connection: close) on a
 * socket, used to answer malformed requests before dropping them.
 * @param msg Plain-text body; NULL uses the status reason phrase.
 */
void cwist_http_send_error_response(int fd, int status, const char *msg);
/** @} */

/** @name Request Data Processing */
/** @{ */
cwist_sstring* cwist_get_client_ip_from_fd(int fd);

/**
 * @brief Format a time_t as an HTTP-date (RFC 7231).
 */
void cwist_http_format_date(time_t t, char *buf, size_t len);

/**
 * @brief Parse an HTTP-date string into a time_t.
 */
time_t cwist_http_parse_date(const char *str);
/** @} */

/** @name Response Lifecycle */
/** @{ */

/**
 * @brief Create a new HTTP response object.
 */
cwist_http_response *cwist_http_response_create(void);
void cwist_http_response_destroy(cwist_http_response *res);

/**
 * @brief Sets a direct pointer for the response body (Zero Copy).
 * Use this when serving large files from memory mapped regions.
 * The pointer must remain valid until the response is sent.
 */
void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len);
void cwist_http_response_set_body_ptr_managed(cwist_http_response *res, const void *ptr, size_t len, cwist_http_body_cleanup_fn cleanup, void *ctx);

/**
 * @brief Build a string representation of an HTTP response.
 *
 * Supported API. Materializes the response status line, headers, and body
 * into a single contiguous string. Intended for debugging and logging; it
 * is not the wire-send path (see cwist_http_send_response()).
 *
 * @param res Response object to stringify. Must not be NULL.
 * @return Heap-allocated string of the full response, or NULL on invalid
 *         input or allocation failure. Caller owns the returned string.
 */
cwist_sstring *cwist_http_stringify_response(cwist_http_response *res);
cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res);
/**
 * @brief Send only the status line and headers of a response (HEAD replies).
 * Content-Length still reflects the would-be body; body resources are
 * released without being transmitted.
 */
cwist_error_t cwist_http_send_response_head(int client_fd, cwist_http_response *res);
/**
 * @brief Whether the TCP_CORK coalescing layer is active for cleartext
 * HTTP/1.1 responses. Enabled at runtime with CWIST_USE_TCP_CORK=1 (burst
 * size via CWIST_TCP_CORK_BURST, default 256 KiB); always false off-Linux.
 */
bool cwist_tcp_cork_enabled(void);
/**
 * @brief Serialize only the status line and headers into a caller buffer.
 * @return Number of bytes written.
 */
size_t cwist_http_serialize_headers(cwist_http_response *res, char *buf, size_t buf_size);

/** @} */

/** @name Header Manipulation */
/** @{ */

/**
 * @brief Add a header to the list.
 * @return INT16 0 on success; INT16 -1 when head is NULL or key/value contains
 *         CR or LF (nothing is added); a JSON error on allocation failure.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key,
                                    const char *value);

/**
 * @brief Find a header value by key.
 * @return Raw char* for convenience, NULL if not found.
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key);

/**
 * @brief Add default security headers (CSP, X-Frame-Options, etc.) if missing.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res);

/**
 * @brief Add Strict-Transport-Security to a TLS response (HTTPS only).
 *
 * RFC 6797 section 7.2 prohibits HSTS over plain HTTP.  Call this from HTTPS
 * handlers after cwist_http_response_add_security_headers(). No-op when
 * the header is already present.
 */
void cwist_http_header_free_all(cwist_http_header_node *head);

/**
 * @brief Add default security headers (CSP, X-Frame-Options, etc.) if missing.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res);
/** @} */

/** @name Helpers */
/** @{ */

/**
 * @brief Convert method enum to string.
 */
const char *cwist_http_method_to_string(cwist_http_method_t method);

/**
 * @brief Convert method string to enum.
 */
cwist_http_method_t cwist_http_string_to_method(const char *method_str);

/** @} */

/** @name TCP Socket Helpers */
/** @{ */

/**
 * @brief Create an IPv4 socket and bind/listen.
 */
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port,
                           uint16_t backlog);

/**
 * @brief Accept sockets and invoke a handler callback.
 */
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4,
                                  void (*handler_func)(int client_fd, void *ctx), void *ctx);

typedef struct cwist_server_config {
    bool use_forking;     ///< Process per request
    bool use_threading;   ///< Thread per request
    bool use_epoll;       ///< Use epoll for accepting
} cwist_server_config;

cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config,
                                     void (*handler)(int, void *), void *ctx);
int headers_have_content_length(cwist_http_header_node *headers);

int cwist_http_pool_init(void);
void cwist_http_pool_limit_core(unsigned int limit);
void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx);
bool cwist_http_pool_rearm_current(int client_fd, void (*handler)(int, void *), void *ctx);
void cwist_http_pool_destroy(void);

/* --- Event-driven (one-shot) connection path for the C1M reactor ---------
 * The classic pool handler parks a worker thread on each keep-alive
 * connection, capping concurrent connections at the thread count.  The async
 * path below never blocks on a read: the callback drains whatever arrived,
 * serves every complete request, and rearms the fd, so one thread can hold
 * hundreds of thousands of mostly-idle connections.
 *
 * Writes on the deferred-completion path are resumable: when a slow client
 * saturates the socket, cwist_http_async_send_response deep-copies the unsent
 * remainder and parks it on a one-shot POLLOUT slot instead of blocking the
 * reactor thread in a poll() wait.  Synchronous (non-deferred) responses
 * still use the bounded poll wait inside cwist_http_send_response
 * (CWIST_HTTP_TIMEOUT_MS). */
typedef enum {
    CWIST_ASYNC_CLOSE = 0,  /* Close fd and release the connection. */
    CWIST_ASYNC_REARM,      /* Keep the connection; wait for more reads. */
    CWIST_ASYNC_DETACH,     /* Handler took ownership of fd (h2c, upgrades). */
    CWIST_ASYNC_DEFER       /* Response deferred (cwist_async); leave fd and conn untouched. */
} cwist_async_action_t;

typedef cwist_async_action_t (*cwist_async_handler_t)(int fd, struct cwist_http_async_conn *conn);

typedef struct cwist_http_async_conn {
    int fd;
    void *user_ctx;                       /* Owning app context. */
    char *rbuf;                           /* Lazy recv stash; freed while empty. */
    size_t cap;
    size_t len;
    bool virgin;                          /* No bytes seen yet (h2c preface sniff). */
    bool expect_continue_sent;            /* 100 Continue already emitted for the pending request. */
} cwist_http_async_conn_t;

typedef enum {
    CWIST_ASYNC_CLOSE = 0,  /* Close fd and release the connection. */
    CWIST_ASYNC_REARM,      /* Keep the connection; wait for more reads. */
    CWIST_ASYNC_DETACH      /* Handler took ownership of fd (h2c, upgrades). */
} cwist_async_action_t;

typedef cwist_async_action_t (*cwist_async_handler_t)(int fd, cwist_http_async_conn_t *conn);

/* Send a deferred completion's response on the reactor path without ever
 * blocking the reactor thread: speculative non-blocking write first; on a
 * partial write the unsent remainder is deep-copied into an owned buffer and
 * parked on a one-shot POLLOUT slot, which resumes the send and then re-arms
 * (keep_alive) or closes exactly like the synchronous completion.  File-stream
 * bodies keep the existing bounded-blocking send path.  Takes over rearm/close
 * of (client_fd, conn) in all outcomes. */
void cwist_http_async_send_response(int client_fd, cwist_http_response *res,
                                    cwist_reactor_t *reactor, cwist_http_async_conn_t *conn,
                                    bool keep_alive, bool head_only);

typedef enum {
    CWIST_RECV_OK = 0,
    CWIST_RECV_NEED_MORE,   /* Partial request; rearm and wait. */
    CWIST_RECV_FATAL        /* Protocol error / overflow; close. */
} cwist_recv_status_t;

bool cwist_http_pool_submit_async(int client_fd, cwist_async_handler_t handler, void *ctx);
int cwist_http_async_conn_fill(cwist_http_async_conn_t *conn);
cwist_recv_status_t cwist_http_receive_request_nb(cwist_http_async_conn_t *conn, cwist_http_request **out, cwist_http_parse_error_t *err_out);

extern const int CWIST_CREATE_SOCKET_FAILED;
extern const int CWIST_HTTP_UNAVAILABLE_ADDRESS;
extern const int CWIST_HTTP_BIND_FAILED;
extern const int CWIST_HTTP_SETSOCKOPT_FAILED;
extern const int CWIST_HTTP_LISTEN_FAILED;

#endif
