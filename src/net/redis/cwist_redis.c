/**
 * @file cwist_redis.c
 * @brief Minimal RESP2 Redis client with connection pooling.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define CWIST_REDIS_BUF_SIZE 65536
#define CWIST_REDIS_LINE_MAX 8192
/* Maximum accepted bulk-string byte count (512 MiB). Redis's own protocol
 * limit is 512 MiB; cap here so a rogue server can't force a huge alloc. */
#define CWIST_REDIS_MAX_BULK_BYTES ((long long)(512 * 1024 * 1024))
/* Reply-tree guards: XREADGROUP-style replies nest ~4 levels; anything far
 * beyond that (or an absurd element count) means a rogue server. */
#define CWIST_REDIS_MAX_REPLY_DEPTH 16
#define CWIST_REDIS_MAX_REPLY_ELEMENTS (1 << 20)

/* Limits for the deadline-bounded pooled command. Tighter than the legacy
 * parser: a timed caller (e.g. a per-request rate limiter) never needs huge
 * replies, and every byte must arrive inside the caller's budget anyway. */
#define CWIST_REDIS_TIMED_MAX_ARGC ((size_t)1 << 20)
#define CWIST_REDIS_TIMED_MAX_FRAME ((size_t)512 * 1024 * 1024)
#define CWIST_REDIS_TIMED_MAX_DEPTH 8
#define CWIST_REDIS_TIMED_MAX_NODES ((size_t)65536)
#define CWIST_REDIS_TIMED_MAX_BULK ((size_t)8 * 1024 * 1024)
#define CWIST_REDIS_TIMED_MAX_PAYLOAD ((size_t)16 * 1024 * 1024)

/* Timed sends never raise SIGPIPE (Apple sockets also get SO_NOSIGPIPE). */
#ifdef MSG_NOSIGNAL
#define CWIST_REDIS_NOSIGNAL MSG_NOSIGNAL
#else
#define CWIST_REDIS_NOSIGNAL 0
#endif

struct cwist_redis {
    int fd;
    char *host;
    int port;
    char recv_buf[CWIST_REDIS_BUF_SIZE];
    size_t recv_len;
    pthread_mutex_t mtx;
};

struct cwist_redis_pool {
    char *host;
    int port;
    size_t max_conns;
    cwist_redis_t **conns;
    bool *available;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    /* Process that created the pool; timed commands from any other process
     * (a fork() child) are rejected because the sockets are shared. */
    pid_t owner_pid;
    /* Peer address resolved at creation, reused for reconnects so request
     * paths never perform DNS. peer_len == 0 means unknown (no reconnect). */
    struct sockaddr_storage peer;
    socklen_t peer_len;
    /* Wake pipe for timed acquirers: they poll() the read end because
     * condition-variable and nanosleep timeouts can be coalesced by tens of
     * milliseconds (observed ~70-150 ms on background-QoS macOS) while poll()
     * timeouts stay precise. Releases write one byte while timed_waiters > 0.
     * -1 when unavailable; timed waits then fall back to the condition. */
    int wake_rd;
    int wake_wr;
    size_t timed_waiters;
};

/* --- Socket helpers ----------------------------------------------------- */

/** Open a blocking TCP connection to the Redis server.
 * @param host Server host name or IPv4 address.
 * @param port Server port.
 * @return Connected socket fd on success, -1 on resolution/connection failure. */
static int redis_open_socket(const char *host, int port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// codeql[cpp/cleartext-transmission]
/** Send a buffer fully over the socket, retrying on EINTR.
 * @return 0 if all bytes were sent, -1 on send error or disconnect. */
static int redis_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        /* RESP speaks plaintext TCP by design (same as redis-cli without
         * --tls). Credentials must be protected at the deployment layer:
         * loopback/Unix sockets, private network, or a TLS-terminating proxy
         * such as stunnel/spiped in front of Redis. */
        ssize_t n = send(fd, buf + sent, len - sent, 0); // codeql[cpp/cleartext-transmission]
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/** Read one CRLF-terminated RESP line from the connection's buffered stream,
 * stripping the trailing "\r\n".
 * @param line Output buffer of @p line_len bytes.
 * @return 0 on success, -1 on I/O error, disconnect, or line longer than
 *         @p line_len - 1 bytes. Not thread-safe by itself; callers must hold
 *         the connection mutex. */
static int redis_recv_line(cwist_redis_t *r, char *line, size_t line_len) {
    size_t i = 0;
    while (1) {
        if (r->recv_len > 0) {
            while (r->recv_len > 0 && i < line_len - 1) {
                char c = r->recv_buf[0];
                /* simple shift */
                memmove(r->recv_buf, r->recv_buf + 1, r->recv_len - 1);
                r->recv_len--;
                if (c == '\n') {
                    if (i > 0 && line[i - 1] == '\r') i--;
                    line[i] = '\0';
                    return 0;
                }
                line[i++] = c;
            }
            if (i >= line_len - 1) return -1;
        }
        ssize_t n =
            recv(r->fd, r->recv_buf + r->recv_len, CWIST_REDIS_BUF_SIZE - r->recv_len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        r->recv_len += (size_t)n;
    }
}

/** Read exactly @p len raw bytes from the connection's buffered stream.
 * @return 0 on success, -1 on I/O error or disconnect before @p len bytes
 *         were read. Not thread-safe by itself; callers must hold the
 *         connection mutex. */
static int redis_recv_bytes(cwist_redis_t *r, char *out, size_t len) {
    size_t got = 0;
    while (got < len) {
        if (r->recv_len > 0) {
            size_t take = r->recv_len < (len - got) ? r->recv_len : (len - got);
            memcpy(out + got, r->recv_buf, take);
            memmove(r->recv_buf, r->recv_buf + take, r->recv_len - take);
            r->recv_len -= take;
            got += take;
            continue;
        }
        ssize_t n =
            recv(r->fd, r->recv_buf + r->recv_len, CWIST_REDIS_BUF_SIZE - r->recv_len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        r->recv_len += (size_t)n;
    }
    return 0;
}

/* --- Deadline-bounded socket helpers ------------------------------------ */

/** Current CLOCK_MONOTONIC time in nanoseconds. */
static uint64_t redis_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** Milliseconds left before @p deadline, rounded up and clamped to INT_MAX;
 * 0 once the deadline has passed. */
static int redis_remaining_ms(uint64_t deadline) {
    uint64_t now = redis_now_ns();
    if (now >= deadline) return 0;
    uint64_t ms = (deadline - now + 999999u) / 1000000u;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

/** Build a CWIST_ERR_INT16 error carrying a (negative errno) code. */
static cwist_error_t redis_code_error(int code) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = (int16_t)code;
    return err;
}

/** Wait until @p fd reports @p events (or an error/hang-up, which the next
 * I/O call surfaces) without passing @p deadline.
 * @return 0 when ready, -ETIMEDOUT, or -EIO when poll() itself fails. */
static int redis_wait_fd(int fd, short events, uint64_t deadline) {
    for (;;) {
        int ms = redis_remaining_ms(deadline);
        if (ms <= 0) return -ETIMEDOUT;
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int n = poll(&p, 1, ms);
        if (n > 0) return 0;
        if (n < 0 && errno != EINTR) return -EIO;
    }
}

/** Write a whole frame before @p deadline without SIGPIPE. @p fd must be in
 * O_NONBLOCK mode: MSG_DONTWAIT is not honoured by send() on every platform
 * (XNU ignores it and blocks on a full send buffer).
 * @return 0, -ETIMEDOUT, or -EIO. A non-zero result may leave a partial
 *         frame on the wire, so the caller must discard the connection. */
static int redis_send_deadline(int fd, const char *buf, size_t len, uint64_t deadline) {
    size_t sent = 0;
    while (sent < len) {
        if (redis_now_ns() >= deadline) return -ETIMEDOUT;
        ssize_t n = send(fd, buf + sent, len - sent, CWIST_REDIS_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
            int rc = redis_wait_fd(fd, POLLOUT, deadline);
            if (rc != 0) return rc;
            continue;
        }
        return -EIO;
    }
    return 0;
}

/** Append at least one byte to the receive buffer before @p deadline.
 * r->fd must be in O_NONBLOCK mode.
 * @return 0, -ETIMEDOUT, -EIO (error or peer EOF), or -EPROTO (buffer full). */
static int redis_fill_deadline(cwist_redis_t *r, uint64_t deadline) {
    if (r->recv_len >= CWIST_REDIS_BUF_SIZE) return -EPROTO;
    for (;;) {
        if (redis_now_ns() >= deadline) return -ETIMEDOUT;
        ssize_t n = recv(r->fd, r->recv_buf + r->recv_len, CWIST_REDIS_BUF_SIZE - r->recv_len, 0);
        if (n > 0) {
            r->recv_len += (size_t)n;
            return 0;
        }
        if (n == 0) return -EIO;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int rc = redis_wait_fd(r->fd, POLLIN, deadline);
            if (rc != 0) return rc;
            continue;
        }
        return -EIO;
    }
}

/** Drop @p n consumed bytes from the front of the receive buffer. */
static void redis_consume(cwist_redis_t *r, size_t n) {
    memmove(r->recv_buf, r->recv_buf + n, r->recv_len - n);
    r->recv_len -= n;
}

/** Ensure one complete CRLF line sits at the front of the receive buffer.
 * On success *@p out_len is the line length without CRLF; the caller reads
 * r->recv_buf[0..len) and then consumes len + 2 bytes. Lines longer than
 * CWIST_REDIS_LINE_MAX - 1 bytes, bare LF terminators, and stray CR bytes
 * are protocol errors. */
static int redis_peek_line(cwist_redis_t *r, uint64_t deadline, size_t *out_len) {
    size_t scanned = 0;
    for (;;) {
        char *nl = memchr(r->recv_buf + scanned, '\n', r->recv_len - scanned);
        if (nl) {
            size_t end = (size_t)(nl - r->recv_buf);
            if (end == 0 || r->recv_buf[end - 1] != '\r') return -EPROTO;
            size_t len = end - 1;
            if (len >= CWIST_REDIS_LINE_MAX) return -EPROTO;
            if (memchr(r->recv_buf, '\r', len)) return -EPROTO;
            *out_len = len;
            return 0;
        }
        scanned = r->recv_len;
        if (r->recv_len > CWIST_REDIS_LINE_MAX) return -EPROTO;
        int rc = redis_fill_deadline(r, deadline);
        if (rc != 0) return rc;
    }
}

/** Copy exactly @p len payload bytes into @p out before @p deadline. */
static int redis_read_exact(cwist_redis_t *r, char *out, size_t len, uint64_t deadline) {
    size_t got = 0;
    while (got < len) {
        if (r->recv_len == 0) {
            int rc = redis_fill_deadline(r, deadline);
            if (rc != 0) return rc;
        }
        size_t take = r->recv_len < len - got ? r->recv_len : len - got;
        memcpy(out + got, r->recv_buf, take);
        redis_consume(r, take);
        got += take;
    }
    return 0;
}

/** Strict RESP integer: optional '-', then 1+ decimal digits, no overflow. */
static bool redis_parse_ll(const char *s, size_t n, long long *out) {
    size_t i = 0;
    bool neg = n > 0 && s[0] == '-';
    if (neg) i = 1;
    if (i >= n) return false;
    unsigned long long limit =
        neg ? (unsigned long long)LLONG_MAX + 1u : (unsigned long long)LLONG_MAX;
    unsigned long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (limit - d) / 10u) return false;
        v = v * 10u + d;
    }
    if (!neg)
        *out = (long long)v;
    else if (v == (unsigned long long)LLONG_MAX + 1u)
        *out = LLONG_MIN;
    else
        *out = -(long long)v;
    return true;
}

/** Close a pooled connection's socket so no later caller can read a stale or
 * partial reply from it. The handle stays in its slot with fd == -1 and is
 * reconnected (to the cached peer) by the next user. */
static void redis_conn_discard(cwist_redis_t *r) {
    if (r->fd >= 0) {
        shutdown(r->fd, SHUT_RDWR);
        close(r->fd);
    }
    r->fd = -1;
    r->recv_len = 0;
}

/** True when an idle connection is not in a clean request state: bytes are
 * pending (an unsolicited/late reply) or the peer hung up/errored. */
static bool redis_conn_unclean(const cwist_redis_t *r) {
    if (r->recv_len != 0) return true;
    struct pollfd p = {.fd = r->fd, .events = POLLIN, .revents = 0};
    int n;
    do {
        n = poll(&p, 1, 0);
    } while (n < 0 && errno == EINTR);
    return n != 0;
}

/** Open a new socket to the pool's cached peer address (no DNS), bounded by
 * @p deadline, and install it in @p r. The socket is left in blocking mode
 * so the legacy APIs keep working on it.
 * @return 0, -ETIMEDOUT, or -EIO (no cached peer, socket/connect failure). */
static int redis_reconnect(const cwist_redis_pool_t *pool, cwist_redis_t *r, uint64_t deadline) {
    if (pool->peer_len == 0) return -EIO;
    int fd = socket(pool->peer.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -EIO;
    int rc = -EIO;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) goto out;
    if (connect(fd, (const struct sockaddr *)&pool->peer, pool->peer_len) != 0) {
        if (errno != EINPROGRESS && errno != EINTR) goto out;
        rc = redis_wait_fd(fd, POLLOUT, deadline);
        if (rc != 0) goto out;
        rc = -EIO;
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) goto out;
    }
    if (fcntl(fd, F_SETFL, flags) != 0) goto out;
    r->fd = fd;
    r->recv_len = 0;
    return 0;
out:
    close(fd);
    return rc;
}

/* --- RESP2 helpers ------------------------------------------------------ */

void cwist_redis_reply_free(cwist_redis_reply_t *reply);

/** Append one RESP2 bulk string ("$<len>\r\n<payload>\r\n") to a frame being
 * built. A NULL @p s is encoded as an empty string.
 * @return Always 0 (appends to the sstring unconditionally). */
static int write_bulk_string(cwist_sstring *out, const char *s) {
    char prefix[32];
    size_t len = s ? strlen(s) : 0;
    snprintf(prefix, sizeof(prefix), "$%zu\r\n", len);
    cwist_sstring_append(out, prefix);
    if (len) cwist_sstring_append(out, s);
    cwist_sstring_append(out, "\r\n");
    return 0;
}

/** Flatten a reply tree into the legacy out_value/out_len contract. */
static void flatten_reply(const cwist_redis_reply_t *tree, char **out_value, size_t *out_len) {
    if (out_value) *out_value = NULL;
    if (out_len) *out_len = 0;
    if (!tree) return;
    if (out_value) {
        if ((tree->type == '+' || tree->type == ':') && tree->str) {
            *out_value = cwist_strdup(tree->str);
            if (out_len) *out_len = strlen(tree->str);
        } else if (tree->type == '$' && tree->str) {
            char *buf = cwist_alloc(tree->len + 1);
            if (!buf) return; /* legacy contract: allocation failure yields NULL */
            memcpy(buf, tree->str, tree->len);
            buf[tree->len] = '\0';
            *out_value = buf;
            if (out_len) *out_len = tree->len;
        }
    } else if (out_len && tree->str) {
        *out_len = tree->len;
    }
}

/** Recursive RESP2 reply parser: builds a cwist_redis_reply_t tree. On any
 * failure the partially built tree is freed and *@p out is NULL. */
static cwist_error_t redis_read_resp(cwist_redis_t *r, int depth, cwist_redis_reply_t **out) {
    char line[CWIST_REDIS_LINE_MAX];
    cwist_redis_reply_t *node = cwist_alloc(sizeof(*node));
    if (!node) return make_error(CWIST_ERR_INT16);
    *out = node;
    if (redis_recv_line(r, line, sizeof(line)) != 0) goto fail;
    node->type = line[0];

    switch (line[0]) {
        case '+': /* simple string */
        case '-': /* error */
            node->str = cwist_strdup(line + 1);
            if (!node->str) goto fail;
            node->len = strlen(line + 1);
            /* Legacy behavior: a '-' reply is signalled through the reply
             * tree (type '-') with a zeroed error value. */
            return (line[0] == '-')
                       ? make_error(CWIST_ERR_INT16)
                       : (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};

        case ':': { /* integer */
            char *end = NULL;
            long long v = strtoll(line + 1, &end, 10);
            if (end == line + 1 || *end != '\0') goto fail;
            node->str = cwist_strdup(line + 1);
            if (!node->str) goto fail;
            node->len = strlen(line + 1);
            node->integer = v;
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
        }

        case '$': { /* bulk string */
            char *end = NULL;
            long long blen = strtoll(line + 1, &end, 10);
            if (end == line + 1 || *end != '\0') goto fail;
            if (blen < 0) return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
            /* Guard against a rogue/malicious server advertising a huge bulk
             * string that would exhaust memory before we read a single byte. */
            if (blen > CWIST_REDIS_MAX_BULK_BYTES) goto fail;
            char *buf = cwist_alloc((size_t)blen + 1);
            if (!buf) goto fail;
            if (redis_recv_bytes(r, buf, (size_t)blen) != 0) {
                cwist_free(buf);
                goto fail;
            }
            buf[blen] = '\0';
            /* consume trailing \r\n */
            char crlf[2];
            if (redis_recv_bytes(r, crlf, 2) != 0) {
                cwist_free(buf);
                goto fail;
            }
            node->str = buf;
            node->len = (size_t)blen;
            node->integer = blen;
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
        }

        case '*': { /* array */
            if (depth >= CWIST_REDIS_MAX_REPLY_DEPTH) goto fail;
            char *end = NULL;
            long long count = strtoll(line + 1, &end, 10);
            if (end == line + 1 || *end != '\0' || count < 0 ||
                count > CWIST_REDIS_MAX_REPLY_ELEMENTS)
                goto fail;
            node->elements = (size_t)count;
            if (count > 0) {
                node->element = cwist_alloc_array((size_t)count, sizeof(*node->element));
                if (!node->element) goto fail;
                for (long long i = 0; i < count; i++) {
                    node->element[i] = NULL;
                    cwist_error_t e = redis_read_resp(r, depth + 1, &node->element[i]);
                    if (e.error.err_i16 != 0) goto fail;
                }
            }
            return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
        }

        default: goto fail;
    }

fail:
    cwist_redis_reply_free(*out);
    *out = NULL;
    return make_error(CWIST_ERR_INT16);
}

/** Compatibility wrapper: flatten a reply tree into the legacy
 * out_value/out_len contract (arrays and errors yield NULL, like before). */
static cwist_error_t read_reply(cwist_redis_t *r, char **out_value, size_t *out_len,
                                char *out_type) {
    cwist_redis_reply_t *tree = NULL;
    cwist_error_t err = redis_read_resp(r, 0, &tree);
    if (out_type) *out_type = tree ? (char)tree->type : '\0';
    if (err.error.err_i16 != 0 || !tree) {
        cwist_redis_reply_free(tree);
        return err;
    }
    char *value = NULL;
    size_t len = 0;
    flatten_reply(tree, &value, &len);
    if (out_value) {
        *out_value = value;
        if (out_len) *out_len = len;
    } else {
        cwist_free(value);
        if (out_len) *out_len = len;
    }
    cwist_redis_reply_free(tree);
    return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
}

/** Recursively free a reply tree, including all child elements and string
 * payloads. Safe to call with NULL. */
void cwist_redis_reply_free(cwist_redis_reply_t *reply) {
    if (!reply) return;
    if (reply->element) {
        for (size_t i = 0; i < reply->elements; i++) {
            cwist_redis_reply_free(reply->element[i]);
        }
        cwist_free(reply->element);
    }
    cwist_free(reply->str);
    cwist_free(reply);
}

/** Send a pre-built RESP frame and read the raw reply tree, holding the
 * connection mutex for the whole request/reply pair so concurrent callers
 * cannot consume each other's replies.
 * @return Error value; on success *@p out_tree holds the reply tree (which the
 *         caller must free with cwist_redis_reply_free()). */
static cwist_error_t redis_command_frame_tree(cwist_redis_t *r, const char *frame, size_t frame_len,
                                              cwist_redis_reply_t **out_tree) {
    if (!r || !frame) return make_error(CWIST_ERR_INT16);
    if (out_tree) *out_tree = NULL;
    /* A request/reply pair must be serialized as one critical section.  Keeping
     * send and receive separate allows two callers to consume each other's
     * replies on the same connection. */
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = redis_send_all(r->fd, frame, frame_len) == 0
                            ? redis_read_resp(r, 0, out_tree)
                            : make_error(CWIST_ERR_INT16);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

/** Send a pre-built RESP frame and flatten the reply into the legacy
 * out_value/out_len contract (arrays and errors yield NULL).
 * @return Error value; on success *@p out_value (if requested) is a
 *         heap-allocated value the caller must free. */
static cwist_error_t redis_command_frame(cwist_redis_t *r, const char *frame, size_t frame_len,
                                         char **out_value, size_t *out_len) {
    cwist_redis_reply_t *tree = NULL;
    cwist_error_t err = redis_command_frame_tree(r, frame, frame_len, &tree);
    if (err.error.err_i16 != 0 || !tree) {
        cwist_redis_reply_free(tree);
        if (out_value) *out_value = NULL;
        if (out_len) *out_len = 0;
        return err;
    }
    char *value = NULL;
    size_t len = 0;
    flatten_reply(tree, &value, &len);
    if (out_value) {
        *out_value = value;
        if (out_len) *out_len = len;
    } else {
        cwist_free(value);
        if (out_len) *out_len = len;
    }
    cwist_redis_reply_free(tree);
    return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
}

/**
 * @brief Append to a RESP frame being built. A failed allocation is
 *        reported on the JSON error channel, where err_i8 stays 0, so the
 *        result is checked with cwist_error_is_ok() and then released;
 *        otherwise a truncated command would be sent to the server.
 */
static bool redis_frame_append(cwist_sstring *frame, const void *data, size_t len) {
    cwist_error_t err = cwist_sstring_append_len(frame, (const char *)data, len);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

/** Build a RESP2 frame from an argv array, send it, and return the raw reply
 * tree. Unlike the legacy string API, a Redis '-' error reply is surfaced as
 * a failure with *@p reply set to NULL.
 * @param r Connection handle.
 * @param argc Number of command arguments.
 * @param argv Argument pointers.
 * @param argv_lens Length of each argument in bytes.
 * @param reply Out: reply tree on success (caller frees); always NULL on error.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_command_argv_reply(cwist_redis_t *r, size_t argc, const void *const *argv,
                                             const size_t *argv_lens, cwist_redis_reply_t **reply) {
    if (!r || !argc || !argv || !argv_lens || !reply) return make_error(CWIST_ERR_INT16);
    *reply = NULL;
    cwist_sstring *frame = cwist_sstring_create();
    if (!frame) return make_error(CWIST_ERR_INT16);
    char count[32];
    snprintf(count, sizeof(count), "*%zu\r\n", argc);
    if (!redis_frame_append(frame, count, strlen(count))) goto fail;
    for (size_t i = 0; i < argc; ++i) {
        if (!argv[i] && argv_lens[i]) goto fail;
        char len[32];
        snprintf(len, sizeof(len), "$%zu\r\n", argv_lens[i]);
        if (!redis_frame_append(frame, len, strlen(len)) ||
            (argv_lens[i] && !redis_frame_append(frame, argv[i], argv_lens[i])) ||
            !redis_frame_append(frame, "\r\n", 2))
            goto fail;
    }
    cwist_error_t err = redis_command_frame_tree(r, frame->data, frame->size, reply);
    cwist_sstring_destroy(frame);
    /* Unlike the legacy string API, surface Redis '-' error replies as
     * failure so callers can tell a server error from an empty array. */
    if (cwist_error_is_ok(&err) && *reply && (*reply)->type == '-') {
        cwist_redis_reply_free(*reply);
        *reply = NULL;
        return make_error(CWIST_ERR_INT16);
    }
    return err;
fail:
    cwist_sstring_destroy(frame);
    return make_error(CWIST_ERR_INT16);
}

/* --- Connection lifecycle ----------------------------------------------- */

/** Open a TCP connection to a Redis server and allocate the client handle.
 * @param host Server host name or IPv4 address.
 * @param port Server port.
 * @return New connection handle on success, NULL on connection or allocation
 *         failure. */
cwist_redis_t *cwist_redis_connect(const char *host, int port) {
    if (!host || port <= 0) return NULL;
    int fd = redis_open_socket(host, port);
    if (fd < 0) return NULL;

    cwist_redis_t *r = cwist_alloc(sizeof(*r));
    if (!r) {
        close(fd);
        return NULL;
    }
    r->fd = fd;
    r->host = cwist_strdup(host);
    r->port = port;
    r->recv_len = 0;
    pthread_mutex_init(&r->mtx, NULL);
    return r;
}

/** Close a connection and free the handle. Shuts the socket down first to wake
 * a subscriber blocked in recv; this is the documented way to stop
 * cwist_redis_subscribe(). Safe to call with NULL. */
void cwist_redis_close(cwist_redis_t *r) {
    if (!r) return;
    /* shutdown wakes a subscriber blocked in recv before the descriptor is
     * released.  This is also the documented way to stop subscribe(). */
    if (r->fd >= 0) {
        shutdown(r->fd, SHUT_RDWR);
        close(r->fd);
        r->fd = -1;
    }
    cwist_free(r->host);
    pthread_mutex_destroy(&r->mtx);
    cwist_free(r);
}

/* --- Command helpers ---------------------------------------------------- */

/** Read one reply from the connection under the mutex and flatten it into the
 * legacy out_value contract.
 * @return Error value; on success *@p out_value (if non-NULL input) is a
 *         heap-allocated value the caller must free. */
static cwist_error_t recv_reply(cwist_redis_t *r, char **out_value) {
    if (!r) return make_error(CWIST_ERR_INT16);
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = read_reply(r, out_value, NULL, NULL);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

/** Send one raw command under the connection mutex without waiting for a
 * reply (used for SUBSCRIBE-style flows where the reply is read separately).
 * @return Error value indicating success or send failure. */
static cwist_error_t send_command(cwist_redis_t *r, const char *cmd) {
    pthread_mutex_lock(&r->mtx);
    cwist_error_t err = redis_send_all(r->fd, cmd, strlen(cmd)) == 0
                            ? (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0}
                            : make_error(CWIST_ERR_INT16);
    pthread_mutex_unlock(&r->mtx);
    return err;
}

/** Send a raw RESP command string and return the flattened first-payload
 * reply.
 * @param r Connection handle.
 * @param cmd Full RESP-encoded command.
 * @param out Optional out: heap-allocated reply value (caller frees); freed
 *        internally when NULL.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_command(cwist_redis_t *r, const char *cmd, char **out) {
    if (!r || !cmd) return make_error(CWIST_ERR_INT16);
    char *value = NULL;
    cwist_error_t err = redis_command_frame(r, cmd, strlen(cmd), &value, NULL);
    if (out)
        *out = value;
    else
        cwist_free(value);
    return err;
}

/** Build a RESP2 frame from an argv array, send it, and return the flattened
 * first-payload reply with its length.
 * @param r Connection handle.
 * @param argc Number of command arguments.
 * @param argv Argument pointers.
 * @param argv_lens Length of each argument in bytes.
 * @param out Optional out: heap-allocated reply value (caller frees).
 * @param out_len Optional out: reply length in bytes.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_command_argv(cwist_redis_t *r, size_t argc, const void *const *argv,
                                       const size_t *argv_lens, char **out, size_t *out_len) {
    if (!r || !argc || !argv || !argv_lens) return make_error(CWIST_ERR_INT16);
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    cwist_sstring *frame = cwist_sstring_create();
    if (!frame) return make_error(CWIST_ERR_INT16);
    char count[32];
    snprintf(count, sizeof(count), "*%zu\r\n", argc);
    if (!redis_frame_append(frame, count, strlen(count))) goto fail;
    for (size_t i = 0; i < argc; ++i) {
        if (!argv[i] && argv_lens[i]) goto fail;
        char len[32];
        snprintf(len, sizeof(len), "$%zu\r\n", argv_lens[i]);
        if (!redis_frame_append(frame, len, strlen(len)) ||
            (argv_lens[i] && !redis_frame_append(frame, argv[i], argv_lens[i])) ||
            !redis_frame_append(frame, "\r\n", 2))
            goto fail;
    }
    char *value = NULL;
    cwist_error_t err = redis_command_frame(r, frame->data, frame->size, &value, out_len);
    cwist_sstring_destroy(frame);
    if (out)
        *out = value;
    else
        cwist_free(value);
    return err;
fail:
    cwist_sstring_destroy(frame);
    return make_error(CWIST_ERR_INT16);
}

/** Authenticate against the server with ACL-style (username, password) or
 * legacy (password-only) AUTH.
 * @param r Connection handle.
 * @param username Optional ACL username; NULL selects password-only AUTH.
 * @param password Password (required).
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_auth(cwist_redis_t *r, const char *username, const char *password) {
    if (!r || !password) return make_error(CWIST_ERR_INT16);
    char *reply = NULL;
    cwist_error_t err;
    if (username) {
        const void *args[3] = {"AUTH", username, password};
        size_t lengths[3] = {4, strlen(username), strlen(password)};
        err = cwist_redis_command_argv(r, 3, args, lengths, &reply, NULL);
    } else {
        const void *args[2] = {"AUTH", password};
        size_t lengths[2] = {4, strlen(password)};
        err = cwist_redis_command_argv(r, 2, args, lengths, &reply, NULL);
    }
    cwist_free(reply);
    return err;
}

/** Select the logical database for subsequent commands (SELECT).
 * @param r Connection handle.
 * @param database Database index.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_select(cwist_redis_t *r, unsigned int database) {
    char db[16];
    snprintf(db, sizeof(db), "%u", database);
    const void *args[] = {"SELECT", db};
    size_t lengths[] = {6, strlen(db)};
    char *reply = NULL;
    cwist_error_t err = cwist_redis_command_argv(r, 2, args, lengths, &reply, NULL);
    cwist_free(reply);
    return err;
}

/** Fetch the value of a key (GET).
 * @param r Connection handle.
 * @param key Key to read.
 * @param out_value Out: heap-allocated value on success, NULL when the key is
 *        missing (caller frees).
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_get(cwist_redis_t *r, const char *key, char **out_value) {
    if (!r || !key || !out_value) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*2\r\n");
    write_bulk_string(cmd, "GET");
    write_bulk_string(cmd, key);
    cwist_error_t err = cwist_redis_command(r, cmd->data, out_value);
    cwist_sstring_destroy(cmd);
    return err;
}

/** Set the value of a key (SET).
 * @param r Connection handle.
 * @param key Key to write.
 * @param value Value to store.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_set(cwist_redis_t *r, const char *key, const char *value) {
    if (!r || !key || !value) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "SET");
    write_bulk_string(cmd, key);
    write_bulk_string(cmd, value);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/** Set the value of a key with a time-to-live (SETEX).
 * @param r Connection handle.
 * @param key Key to write.
 * @param value Value to store.
 * @param seconds TTL in seconds; must be positive.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_setex(cwist_redis_t *r, const char *key, const char *value, int seconds) {
    if (!r || !key || !value || seconds <= 0) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*4\r\n");
    write_bulk_string(cmd, "SETEX");
    write_bulk_string(cmd, key);
    char sec[32];
    snprintf(sec, sizeof(sec), "%d", seconds);
    write_bulk_string(cmd, sec);
    write_bulk_string(cmd, value);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/** Delete a key (DEL).
 * @param r Connection handle.
 * @param key Key to delete.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_del(cwist_redis_t *r, const char *key) {
    if (!r || !key) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*2\r\n");
    write_bulk_string(cmd, "DEL");
    write_bulk_string(cmd, key);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/** Set a key's time-to-live in seconds (EXPIRE).
 * @param r Connection handle.
 * @param key Key to expire.
 * @param seconds TTL in seconds; must be positive.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_expire(cwist_redis_t *r, const char *key, int seconds) {
    if (!r || !key || seconds <= 0) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "EXPIRE");
    write_bulk_string(cmd, key);
    char sec[32];
    snprintf(sec, sizeof(sec), "%d", seconds);
    write_bulk_string(cmd, sec);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/** Publish a message to a channel (PUBLISH).
 * @param r Connection handle.
 * @param channel Channel to publish to.
 * @param message Message payload.
 * @return Error value indicating success or failure. */
cwist_error_t cwist_redis_publish(cwist_redis_t *r, const char *channel, const char *message) {
    if (!r || !channel || !message) return make_error(CWIST_ERR_INT16);
    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    cwist_sstring_append(cmd, "*3\r\n");
    write_bulk_string(cmd, "PUBLISH");
    write_bulk_string(cmd, channel);
    write_bulk_string(cmd, message);
    char *out = NULL;
    cwist_error_t err = cwist_redis_command(r, cmd->data, &out);
    cwist_free(out);
    cwist_sstring_destroy(cmd);
    return err;
}

/* --- Pub/Sub ------------------------------------------------------------ */

/** Subscribe to channels and run a blocking message loop, invoking the
 * callback for each incoming message.
 * @param r Connection handle.
 * @param channels NULL-terminated list of channel names.
 * @param cb Callback invoked as cb(channel, message, ctx) per message.
 * @param ctx Opaque pointer passed through to the callback.
 * @return Zero error value; the loop ends when the connection is shut down
 *         (see cwist_redis_close()). */
cwist_error_t cwist_redis_subscribe(cwist_redis_t *r, const char **channels, cwist_redis_msg_cb cb,
                                    void *ctx) {
    if (!r || !channels || !channels[0] || !cb) return make_error(CWIST_ERR_INT16);

    cwist_sstring *cmd = cwist_sstring_create();
    if (!cmd) return make_error(CWIST_ERR_INT16);
    size_t count = 0;
    while (channels[count]) count++;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "*%zu\r\n", count + 1);
    cwist_sstring_append(cmd, prefix);
    write_bulk_string(cmd, "SUBSCRIBE");
    for (size_t i = 0; i < count; i++) {
        write_bulk_string(cmd, channels[i]);
    }
    cwist_error_t err = send_command(r, cmd->data);
    cwist_sstring_destroy(cmd);
    if (err.error.err_i16 != 0) return err;

    /* Consume subscription confirmations. */
    for (size_t i = 0; i < count; i++) {
        char *out = NULL;
        err = recv_reply(r, &out);
        cwist_free(out);
        if (err.error.err_i16 != 0) return err;
    }

    /* Blocking read loop for messages. */
    while (1) {
        char line[CWIST_REDIS_LINE_MAX];
        if (redis_recv_line(r, line, sizeof(line)) != 0) break;
        if (line[0] != '*') continue;
        char *arr_end = NULL;
        long long arr_count = strtoll(line + 1, &arr_end, 10);
        if (arr_end == line + 1 || *arr_end != '\0') continue;
        if (arr_count < 3) continue;

        char *type = NULL;
        if (read_reply(r, &type, NULL, NULL).error.err_i16 != 0) break;

        char *channel = NULL;
        if (read_reply(r, &channel, NULL, NULL).error.err_i16 != 0) {
            cwist_free(type);
            break;
        }

        char *message = NULL;
        if (read_reply(r, &message, NULL, NULL).error.err_i16 != 0) {
            cwist_free(type);
            cwist_free(channel);
            break;
        }

        if (type && strcmp(type, "message") == 0) {
            cb(channel, message, ctx);
        }
        cwist_free(type);
        cwist_free(channel);
        cwist_free(message);
    }
    return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = 0};
}

/* --- Connection pool ---------------------------------------------------- */

/** Create a connection pool with max_conns pre-opened connections.
 * @param host Server host name or IPv4 address.
 * @param port Server port.
 * @param max_conns Number of connections to open; must be non-zero.
 * @return New pool handle on success, NULL on any connection or allocation
 *         failure (partial state is cleaned up). */
cwist_redis_pool_t *cwist_redis_pool_create(const char *host, int port, size_t max_conns) {
    if (!host || port <= 0 || max_conns == 0) return NULL;

    cwist_redis_pool_t *pool = cwist_alloc(sizeof(*pool));
    if (!pool) return NULL;
    pool->wake_rd = -1;
    pool->wake_wr = -1;
    pool->host = cwist_strdup(host);
    pool->port = port;
    pool->max_conns = max_conns;
    pool->conns = cwist_alloc_array(max_conns, sizeof(cwist_redis_t *));
    pool->available = cwist_alloc_array(max_conns, sizeof(bool));
    if (!pool->host || !pool->conns || !pool->available) goto fail;

    for (size_t i = 0; i < max_conns; i++) {
        pool->conns[i] = cwist_redis_connect(host, port);
        if (!pool->conns[i]) goto fail;
        pool->available[i] = true;
    }

    /* Cache the resolved peer once so request-time reconnects need no DNS. */
    pool->peer_len = (socklen_t)sizeof(pool->peer);
    if (getpeername(pool->conns[0]->fd, (struct sockaddr *)&pool->peer, &pool->peer_len) != 0)
        pool->peer_len = 0;
    pool->owner_pid = getpid();

    int wake[2];
    if (pipe(wake) == 0) {
        bool ok = true;
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(wake[i], F_GETFL, 0);
            ok = ok && fl >= 0 && fcntl(wake[i], F_SETFL, fl | O_NONBLOCK) == 0 &&
                 fcntl(wake[i], F_SETFD, FD_CLOEXEC) == 0;
        }
        if (ok) {
            pool->wake_rd = wake[0];
            pool->wake_wr = wake[1];
        } else {
            close(wake[0]);
            close(wake[1]);
        }
    }

    pthread_mutex_init(&pool->mtx, NULL);
    pthread_cond_init(&pool->cond, NULL);
    return pool;

fail:
    if (pool) {
        for (size_t i = 0; i < max_conns && pool->conns; i++) {
            if (pool->conns[i]) cwist_redis_close(pool->conns[i]);
        }
        if (pool->wake_rd >= 0) close(pool->wake_rd);
        if (pool->wake_wr >= 0) close(pool->wake_wr);
        cwist_free(pool->conns);
        cwist_free(pool->available);
        cwist_free(pool->host);
        cwist_free(pool);
    }
    return NULL;
}

/** Destroy a pool, closing every connection it owns. Safe to call with NULL. */
void cwist_redis_pool_destroy(cwist_redis_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i]) cwist_redis_close(pool->conns[i]);
    }
    pthread_mutex_unlock(&pool->mtx);
    if (pool->wake_rd >= 0) close(pool->wake_rd);
    if (pool->wake_wr >= 0) close(pool->wake_wr);
    pthread_mutex_destroy(&pool->mtx);
    pthread_cond_destroy(&pool->cond);
    cwist_free(pool->conns);
    cwist_free(pool->available);
    cwist_free(pool->host);
    cwist_free(pool);
}

/** Acquire an idle pool connection, blocking on the condition variable until
 * one becomes available. A slot whose socket was discarded by the timed API
 * is reconnected (blocking, to the cached peer) before it is handed out; if
 * that fails the command fails on the closed handle as before.
 * @return A connection marked in-use; NULL only when @p pool is NULL. */
static cwist_redis_t *redis_pool_acquire(cwist_redis_pool_t *pool) {
    if (!pool) return NULL;
    pthread_mutex_lock(&pool->mtx);
    while (1) {
        for (size_t i = 0; i < pool->max_conns; i++) {
            if (pool->available[i]) {
                pool->available[i] = false;
                pthread_mutex_unlock(&pool->mtx);
                cwist_redis_t *r = pool->conns[i];
                if (r->fd < 0) (void)redis_reconnect(pool, r, UINT64_MAX);
                return r;
            }
        }
        pthread_cond_wait(&pool->cond, &pool->mtx);
    }
}

/** Return a connection to the pool and signal a waiting acquirer (one
 * condition waiter plus, when present, the timed waiters' wake pipe). */
static void redis_pool_release(cwist_redis_pool_t *pool, cwist_redis_t *r) {
    if (!pool || !r) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i] == r) {
            pool->available[i] = true;
            pthread_cond_signal(&pool->cond);
            if (pool->timed_waiters > 0 && pool->wake_wr >= 0) {
                /* Non-blocking; a full pipe is already readable. */
                const char b = 1;
                ssize_t n = write(pool->wake_wr, &b, 1);
                (void)n;
            }
            break;
        }
    }
    pthread_mutex_unlock(&pool->mtx);
}

/** Pool wrapper of cwist_redis_get(): acquires a connection, runs GET, and
 * releases the connection. See cwist_redis_get() for parameter details. */
cwist_error_t cwist_redis_pool_get(cwist_redis_pool_t *pool, const char *key, char **out_value) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_get(r, key, out_value);
    redis_pool_release(pool, r);
    return err;
}

/** Pool wrapper of cwist_redis_set(): acquires a connection, runs SET, and
 * releases the connection. See cwist_redis_set() for parameter details. */
cwist_error_t cwist_redis_pool_set(cwist_redis_pool_t *pool, const char *key, const char *value) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_set(r, key, value);
    redis_pool_release(pool, r);
    return err;
}

/** Pool wrapper of cwist_redis_setex(): acquires a connection, runs SETEX, and
 * releases the connection. See cwist_redis_setex() for parameter details. */
cwist_error_t cwist_redis_pool_setex(cwist_redis_pool_t *pool, const char *key, const char *value,
                                     int seconds) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_setex(r, key, value, seconds);
    redis_pool_release(pool, r);
    return err;
}

/** Pool wrapper of cwist_redis_del(): acquires a connection, runs DEL, and
 * releases the connection. See cwist_redis_del() for parameter details. */
cwist_error_t cwist_redis_pool_del(cwist_redis_pool_t *pool, const char *key) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_del(r, key);
    redis_pool_release(pool, r);
    return err;
}

/** Pool wrapper of cwist_redis_publish(): acquires a connection, runs
 * PUBLISH, and releases the connection. See cwist_redis_publish() for
 * parameter details. */
cwist_error_t cwist_redis_pool_publish(cwist_redis_pool_t *pool, const char *channel,
                                       const char *message) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_publish(r, channel, message);
    redis_pool_release(pool, r);
    return err;
}

/** Pool wrapper of cwist_redis_command_argv(): acquires a connection, runs the
 * argv command, and releases the connection. See cwist_redis_command_argv()
 * for parameter details. */
cwist_error_t cwist_redis_pool_command_argv(cwist_redis_pool_t *pool, size_t argc,
                                            const void *const *argv, const size_t *argv_lens,
                                            char **out, size_t *out_len) {
    cwist_redis_t *r = redis_pool_acquire(pool);
    if (!r) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_redis_command_argv(r, argc, argv, argv_lens, out, out_len);
    redis_pool_release(pool, r);
    return err;
}

/* --- Deadline-bounded pooled command ------------------------------------ */

/** Wait for a release for at most the time left before @p deadline.
 * Caller holds pool->mtx (dropped while sleeping). Wakeups (releases,
 * timeouts, spurious) are all handled by the caller rescanning the slots and
 * re-checking the deadline.
 *
 * Preferred path: poll() the wake pipe. A release that lands between the
 * unlock and poll() leaves a byte in the pipe, so it is never lost. Each
 * wakeup consumes at most one byte so concurrent timed waiters cannot steal
 * each other's notifications; the last waiter out drains leftovers (safe
 * because no waiter can be inside poll() while the count is zero). */
static void redis_pool_wait_until(cwist_redis_pool_t *pool, uint64_t deadline) {
    uint64_t now = redis_now_ns();
    if (now >= deadline) return;
    uint64_t rem = deadline - now;
    if (pool->wake_rd >= 0) {
        uint64_t ms = (rem + 999999u) / 1000000u;
        struct pollfd pfd = {.fd = pool->wake_rd, .events = POLLIN, .revents = 0};
        pool->timed_waiters++;
        pthread_mutex_unlock(&pool->mtx);
        (void)poll(&pfd, 1, ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms);
        pthread_mutex_lock(&pool->mtx);
        pool->timed_waiters--;
        char sink[64];
        if (pool->timed_waiters == 0) {
            while (read(pool->wake_rd, sink, sizeof(sink)) > 0) {
            }
        } else {
            ssize_t n = read(pool->wake_rd, sink, 1);
            (void)n;
        }
        return;
    }
    /* Pipe allocation failed: use short monotonic rechecks, not a
     * realtime condition timer that can jump backwards or be coalesced. */
    int ms = redis_remaining_ms(deadline);
    pthread_mutex_unlock(&pool->mtx);
    if (ms > 0) (void)poll(NULL, 0, ms < 2 ? ms : 2);
    pthread_mutex_lock(&pool->mtx);
}

/** Claim a free slot before @p deadline, preferring one whose socket is
 * still open. Slots are rescanned after every wakeup before the deadline is
 * checked, so a release signal consumed here is never lost: either this
 * caller takes the slot (and later releases it, signalling again) or no
 * slot was free.
 * @return 0 with *@p out set, or -ETIMEDOUT. */
static int redis_pool_acquire_deadline(cwist_redis_pool_t *pool, uint64_t deadline,
                                       cwist_redis_t **out) {
    pthread_mutex_lock(&pool->mtx);
    for (;;) {
        size_t pick = pool->max_conns;
        for (size_t i = 0; i < pool->max_conns; i++) {
            if (!pool->available[i]) continue;
            if (pick == pool->max_conns) pick = i;
            if (pool->conns[i]->fd >= 0) {
                pick = i;
                break;
            }
        }
        if (pick < pool->max_conns) {
            pool->available[pick] = false;
            pthread_mutex_unlock(&pool->mtx);
            *out = pool->conns[pick];
            return 0;
        }
        if (redis_now_ns() >= deadline) {
            pthread_mutex_unlock(&pool->mtx);
            return -ETIMEDOUT;
        }
        redis_pool_wait_until(pool, deadline);
    }
}

/** Number of decimal digits in @p v. */
static size_t redis_digits(size_t v) {
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

/** Validate argv and compute the exact RESP frame size without touching any
 * argument bytes. @return 0 or -EINVAL (bad pointer/length, size limits). */
static int redis_frame_size(size_t argc, const void *const *argv, const size_t *argv_lens,
                            size_t *out) {
    if (argc > CWIST_REDIS_TIMED_MAX_ARGC) return -EINVAL;
    size_t total = 1 + redis_digits(argc) + 2;
    for (size_t i = 0; i < argc; i++) {
        size_t len = argv_lens[i];
        if (!argv[i] && len) return -EINVAL;
        if (len > CWIST_REDIS_TIMED_MAX_FRAME) return -EINVAL;
        size_t part = 1 + redis_digits(len) + 2 + len + 2;
        if (part > CWIST_REDIS_TIMED_MAX_FRAME - total) return -EINVAL;
        total += part;
    }
    *out = total;
    return 0;
}

/** Encode argv into @p frame (sized by redis_frame_size()). */
static void redis_frame_fill(char *frame, size_t frame_len, size_t argc, const void *const *argv,
                             const size_t *argv_lens) {
    char *p = frame;
    char *end = frame + frame_len;
    p += snprintf(p, (size_t)(end - p), "*%zu\r\n", argc);
    for (size_t i = 0; i < argc; i++) {
        /* snprintf writes a NUL after "\r\n"; the payload overwrites it. */
        p += snprintf(p, (size_t)(end - p), "$%zu\r\n", argv_lens[i]);
        if (argv_lens[i]) memcpy(p, argv[i], argv_lens[i]);
        p += argv_lens[i];
        memcpy(p, "\r\n", 2);
        p += 2;
    }
}

typedef struct redis_timed_parse {
    cwist_redis_t *r;
    uint64_t deadline;
    size_t nodes;
    size_t payload;
} redis_timed_parse;

/** Copy @p len bytes into a new NUL-terminated cwist_alloc() string. */
static char *redis_copy_str(const char *src, size_t len) {
    char *s = cwist_alloc(len + 1);
    if (!s) return NULL;
    if (len) memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

/** Strict, limit-checked, deadline-bounded RESP2 parser.
 * @return 0 with *@p out set, or a negative errno with *@p out NULL. Any
 *         failure leaves the stream position undefined (caller discards). */
static int redis_timed_read(redis_timed_parse *ps, int depth, cwist_redis_reply_t **out) {
    *out = NULL;
    if (++ps->nodes > CWIST_REDIS_TIMED_MAX_NODES) return -EPROTO;
    cwist_redis_t *r = ps->r;
    size_t n = 0;
    int rc = redis_peek_line(r, ps->deadline, &n);
    if (rc != 0) return rc;
    if (n == 0) return -EPROTO;
    const char *line = r->recv_buf;
    char type = line[0];
    long long v = 0;
    if ((type == ':' || type == '$' || type == '*') && !redis_parse_ll(line + 1, n - 1, &v))
        return -EPROTO;
    if (type != '+' && type != '-' && type != ':' && type != '$' && type != '*') return -EPROTO;
    if (n - 1 > CWIST_REDIS_TIMED_MAX_PAYLOAD - ps->payload) return -EPROTO;

    cwist_redis_reply_t *node = cwist_alloc(sizeof(*node));
    if (!node) return -ENOMEM;
    node->type = type;
    rc = -EPROTO;
    switch (type) {
        case '+':
        case '-':
        case ':':
            ps->payload += n - 1;
            node->str = redis_copy_str(line + 1, n - 1);
            if (!node->str) {
                rc = -ENOMEM;
                goto fail;
            }
            node->len = n - 1;
            node->integer = type == ':' ? v : 0;
            redis_consume(r, n + 2);
            break;
        case '$': {
            redis_consume(r, n + 2);
            if (v == -1) {
                node->integer = -1;
                break;
            }
            if (v < 0 || (unsigned long long)v > CWIST_REDIS_TIMED_MAX_BULK ||
                (size_t)v > CWIST_REDIS_TIMED_MAX_PAYLOAD - ps->payload)
                goto fail;
            ps->payload += (size_t)v;
            node->str = cwist_alloc((size_t)v + 1);
            if (!node->str) {
                rc = -ENOMEM;
                goto fail;
            }
            rc = redis_read_exact(r, node->str, (size_t)v, ps->deadline);
            if (rc != 0) goto fail;
            node->str[v] = '\0';
            node->len = (size_t)v;
            node->integer = v;
            char crlf[2];
            rc = redis_read_exact(r, crlf, 2, ps->deadline);
            if (rc != 0) goto fail;
            if (crlf[0] != '\r' || crlf[1] != '\n') {
                rc = -EPROTO;
                goto fail;
            }
            break;
        }
        case '*':
            redis_consume(r, n + 2);
            if (v == -1) {
                node->integer = -1;
                break;
            }
            if (v < 0 || depth >= CWIST_REDIS_TIMED_MAX_DEPTH ||
                (unsigned long long)v > CWIST_REDIS_TIMED_MAX_NODES - ps->nodes)
                goto fail;
            node->integer = v;
            if (v > 0) {
                node->element = cwist_alloc_array((size_t)v, sizeof(*node->element));
                if (!node->element) {
                    rc = -ENOMEM;
                    goto fail;
                }
                node->elements = (size_t)v;
                for (size_t i = 0; i < (size_t)v; i++) {
                    node->element[i] = NULL;
                    rc = redis_timed_read(ps, depth + 1, &node->element[i]);
                    if (rc != 0) goto fail;
                }
            }
            break;
    }
    *out = node;
    return 0;
fail:
    cwist_redis_reply_free(node);
    return rc;
}

/** Deadline-bounded pooled command; see cwist_redis.h for the contract. */
cwist_error_t cwist_redis_pool_command_argv_reply_timeout(cwist_redis_pool_t *pool, size_t argc,
                                                          const void *const *argv,
                                                          const size_t *argv_lens,
                                                          unsigned timeout_ms,
                                                          cwist_redis_reply_t **reply) {
    if (reply) *reply = NULL;
    if (!pool || !argc || !argv || !argv_lens || !reply || timeout_ms == 0)
        return redis_code_error(-EINVAL);
    const uint64_t deadline = redis_now_ns() + (uint64_t)timeout_ms * 1000000u;
    /* A fork() child shares the parent's sockets and may hold a copy of a
     * locked mutex: reject before touching any pool state. */
    if (pool->owner_pid != getpid()) return redis_code_error(-EPERM);

    size_t frame_len = 0;
    int rc = redis_frame_size(argc, argv, argv_lens, &frame_len);
    if (rc != 0) return redis_code_error(rc);
    char *frame = cwist_malloc(frame_len + 1);
    if (!frame) return redis_code_error(-ENOMEM);
    redis_frame_fill(frame, frame_len + 1, argc, argv, argv_lens);

    cwist_redis_t *r = NULL;
    rc = redis_pool_acquire_deadline(pool, deadline, &r);
    if (rc != 0) {
        cwist_free(frame);
        return redis_code_error(rc);
    }

    cwist_redis_reply_t *tree = NULL;
    pthread_mutex_lock(&r->mtx);
    if (redis_now_ns() >= deadline) {
        rc = -ETIMEDOUT;
    } else {
        /* Nothing has been sent yet, so replacing a stale, hung-up, or
         * previously discarded connection is not a retry. */
        if (r->fd >= 0 && redis_conn_unclean(r)) redis_conn_discard(r);
        rc = r->fd >= 0 ? 0 : redis_reconnect(pool, r, deadline);
    }
    if (rc == 0) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
        /* No per-send flag: mark the (possibly legacy-created) socket. */
        int nosig = 1;
        (void)setsockopt(r->fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
        /* Non-blocking only for this call; legacy APIs expect blocking fds. */
        int fl = fcntl(r->fd, F_GETFL, 0);
        if (fl < 0 || fcntl(r->fd, F_SETFL, fl | O_NONBLOCK) != 0) rc = -EIO;
        /* From the first written byte on, the command is ambiguous: any
         * failure discards the connection and is never retried. */
        if (rc == 0) rc = redis_send_deadline(r->fd, frame, frame_len, deadline);
        if (rc == 0) {
            redis_timed_parse ps = {.r = r, .deadline = deadline, .nodes = 0, .payload = 0};
            rc = redis_timed_read(&ps, 0, &tree);
        }
        if (rc == 0 && fcntl(r->fd, F_SETFL, fl) != 0) rc = -EIO;
        /* Bytes beyond the reply mean the peer is out of sync with us. */
        if (rc != 0 || r->recv_len != 0) redis_conn_discard(r);
        if (rc == 0 && tree->type == '-') {
            cwist_redis_reply_free(tree);
            tree = NULL;
            rc = -EBADMSG;
        }
    }
    pthread_mutex_unlock(&r->mtx);
    redis_pool_release(pool, r);
    cwist_free(frame);
    if (rc != 0) {
        cwist_redis_reply_free(tree);
        return redis_code_error(rc);
    }
    *reply = tree;
    return redis_code_error(0);
}
