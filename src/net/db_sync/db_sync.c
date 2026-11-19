#define _POSIX_C_SOURCE 200809L
#include <cwist/net/db_sync/db_sync.h>
#include <cwist/security/db_crypt/db_crypt.h>

#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/**
 * @file db_sync.c
 * @brief Minimal TCP protocol for shipping encrypted SQLite snapshots between peers.
 */

static const unsigned char SYNC_MAGIC[4] = {0x43, 0x57, 0x53, 0x59}; /* "CWSY" */
#define DB_SYNC_NONCE_LEN 32
#define DB_SYNC_MAC_LEN 32

#ifndef CWIST_DB_SYNC_MAX_BLOB_SIZE
#define CWIST_DB_SYNC_MAX_BLOB_SIZE ((uint64_t)512 * 1024 * 1024)
#endif
#ifndef CWIST_DB_SYNC_TIMEOUT_SEC
#define CWIST_DB_SYNC_TIMEOUT_SEC 30
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int db_sync_configure_socket(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct timeval timeout = {.tv_sec = CWIST_DB_SYNC_TIMEOUT_SEC, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
               ? 0
               : -1;
}

static int db_sync_connect(const struct addrinfo *addresses) {
    for (const struct addrinfo *ai = addresses; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd);
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && (errno == EINPROGRESS || errno == EINTR)) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            do {
                rc = poll(&pfd, 1, CWIST_DB_SYNC_TIMEOUT_SEC * 1000);
            } while (rc < 0 && errno == EINTR);
            if (rc > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0 ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc != 0 || fcntl(fd, F_SETFL, flags) != 0 || db_sync_configure_socket(fd) != 0) {
            close(fd);
            continue;
        }
        return fd;
    }
    return -1;
}

static uint64_t db_sync_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int db_sync_wait(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = db_sync_now_ms();
        if (now >= deadline) return -1;
        struct pollfd pfd = {.fd = fd, .events = events};
        int rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) return -1;
        return 0;
    }
}

/**
 * @brief Write exactly the requested number of bytes to a socket.
 * @param fd Connected socket descriptor.
 * @param buf Source buffer to write.
 * @param len Number of bytes to send.
 * @return 0 on success, or -1 on short/failed writes.
 */
static int db_sync_write_all(int fd, const void *buf, size_t len, uint64_t deadline) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        if (db_sync_wait(fd, POLLOUT, deadline) != 0) return -1;
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/**
 * @brief Read exactly the requested number of bytes from a socket.
 * @param fd Connected socket descriptor.
 * @param buf Destination buffer to fill.
 * @param len Number of bytes to read.
 * @return 0 on success, or -1 on short/failed reads.
 */
static int read_all(int fd, void *buf, size_t len, uint64_t deadline) {
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        if (db_sync_wait(fd, POLLIN, deadline) != 0) return -1;
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int db_sync_mac(const cwist_db_crypt_ctx_t *ctx, const char *domain,
                       const unsigned char client_nonce[DB_SYNC_NONCE_LEN],
                       const unsigned char server_nonce[DB_SYNC_NONCE_LEN],
                       const unsigned char *data, size_t data_len,
                       unsigned char out[DB_SYNC_MAC_LEN]) {
    static const unsigned char zero_nonce[DB_SYNC_NONCE_LEN];
    HMAC_CTX *mac = HMAC_CTX_new();
    if (!mac) return -1;
    unsigned int out_len = 0;
    int ok = HMAC_Init_ex(mac, ctx->kek, sizeof(ctx->kek), EVP_sha256(), NULL) == 1 &&
             HMAC_Update(mac, (const unsigned char *)domain, strlen(domain)) == 1 &&
             HMAC_Update(mac, client_nonce, DB_SYNC_NONCE_LEN) == 1 &&
             HMAC_Update(mac, server_nonce ? server_nonce : zero_nonce, DB_SYNC_NONCE_LEN) == 1 &&
             (!data_len || HMAC_Update(mac, data, data_len) == 1) &&
             HMAC_Final(mac, out, &out_len) == 1 && out_len == DB_SYNC_MAC_LEN;
    HMAC_CTX_free(mac);
    return ok ? 0 : -1;
}

/**
 * @brief Serialize and encrypt a SQLite database, then serve it to one TCP client.
 * @param db SQLite connection whose main database should be transferred.
 * @param ctx Encryption context used to seal the serialized bytes.
 * @param port TCP port to listen on, or the default when <= 0.
 * @return CWIST_DB_SYNC_OK on success, or a protocol/network/crypto error code.
 */
int cwist_db_sync_serve(sqlite3 *db, const cwist_db_crypt_ctx_t *ctx, int port) {
    if (!db || !ctx) return CWIST_DB_SYNC_ERR_ARGS;
    if (port <= 0) port = CWIST_DB_SYNC_DEFAULT_PORT;

    /* 1. Serialise the database to a byte buffer. */
    sqlite3_int64 db_size = 0;
    unsigned char *db_bytes = sqlite3_serialize(db, "main", &db_size, 0);
    if (!db_bytes || db_size <= 0) {
        fprintf(stderr, "[db_sync] serialize failed\n");
        return CWIST_DB_SYNC_ERR_MEM;
    }

    /* 2. Seal the bytes. */
    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(ctx, db_bytes, (size_t)db_size, &blob_len);
    sqlite3_free(db_bytes);

    if (!blob) {
        fprintf(stderr, "[db_sync] seal failed\n");
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }

    /* 3. Create a listening TCP socket. */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[db_sync] bind port %d failed: %s\n", port, strerror(errno));
        close(srv_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    if (listen(srv_fd, 1) < 0) {
        close(srv_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    uint64_t deadline = db_sync_now_ms() + CWIST_DB_SYNC_TIMEOUT_SEC * 1000;
    int cli_fd = -1;
    unsigned char client_nonce[DB_SYNC_NONCE_LEN];
    for (;;) {
        if (db_sync_wait(srv_fd, POLLIN, deadline) != 0) break;
        cli_fd = accept(srv_fd, NULL, NULL);
        if (cli_fd < 0) continue;
        if (db_sync_configure_socket(cli_fd) != 0) {
            close(cli_fd);
            cli_fd = -1;
            continue;
        }
        unsigned char hello[4 + DB_SYNC_NONCE_LEN + DB_SYNC_MAC_LEN];
        unsigned char expected[DB_SYNC_MAC_LEN];
        if (read_all(cli_fd, hello, sizeof(hello), deadline) == 0 &&
            memcmp(hello, SYNC_MAGIC, 4) == 0) {
            memcpy(client_nonce, hello + 4, DB_SYNC_NONCE_LEN);
            if (db_sync_mac(ctx, "CWSY client", client_nonce, NULL, NULL, 0, expected) == 0 &&
                CRYPTO_memcmp(expected, hello + 4 + DB_SYNC_NONCE_LEN, DB_SYNC_MAC_LEN) == 0)
                break;
        }
        close(cli_fd);
        cli_fd = -1;
    }
    close(srv_fd);
    if (cli_fd < 0) {
        free(blob);
        return CWIST_DB_SYNC_ERR_PROTO;
    }

    unsigned char server_nonce[DB_SYNC_NONCE_LEN];
    unsigned char server_auth[DB_SYNC_MAC_LEN];
    unsigned char blob_auth[DB_SYNC_MAC_LEN];
    unsigned char len_buf[8];
    for (int i = 0; i < 8; i++) len_buf[i] = (unsigned char)((uint64_t)blob_len >> (8 * i));
    if (RAND_bytes(server_nonce, sizeof(server_nonce)) != 1 ||
        db_sync_mac(ctx, "CWSY server", client_nonce, server_nonce, NULL, 0, server_auth) != 0 ||
        db_sync_mac(ctx, "CWSY blob", client_nonce, server_nonce, blob, blob_len, blob_auth) != 0) {
        close(cli_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }
    if (db_sync_write_all(cli_fd, server_nonce, sizeof(server_nonce), deadline) < 0 ||
        db_sync_write_all(cli_fd, server_auth, sizeof(server_auth), deadline) < 0 ||
        db_sync_write_all(cli_fd, len_buf, sizeof(len_buf), deadline) < 0 ||
        db_sync_write_all(cli_fd, blob, blob_len, deadline) < 0 ||
        db_sync_write_all(cli_fd, blob_auth, sizeof(blob_auth), deadline) < 0) {
        close(cli_fd);
        free(blob);
        return CWIST_DB_SYNC_ERR_NET;
    }

    close(cli_fd);
    free(blob);
    return CWIST_DB_SYNC_OK;
}

/**
 * @brief Connect to a sync server, download the encrypted snapshot, and decrypt it.
 * @param host Remote host name or address to connect to.
 * @param port Remote TCP port, or the default when <= 0.
 * @param ctx Encryption context used to decrypt the transferred blob.
 * @param out_bytes Output pointer that receives the decrypted SQLite image.
 * @param out_len Output pointer that receives the decrypted byte count.
 * @return CWIST_DB_SYNC_OK on success, or a protocol/network/crypto error code.
 */
int cwist_db_sync_pull(const char *host, int port, const cwist_db_crypt_ctx_t *ctx,
                       unsigned char **out_bytes, size_t *out_len) {
    if (!host || !ctx || !out_bytes || !out_len) return CWIST_DB_SYNC_ERR_ARGS;
    if (port <= 0) port = CWIST_DB_SYNC_DEFAULT_PORT;
    uint64_t deadline = db_sync_now_ms() + CWIST_DB_SYNC_TIMEOUT_SEC * 1000;

    /* 1. Resolve host. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[db_sync] getaddrinfo failed for %s\n", host);
        return CWIST_DB_SYNC_ERR_NET;
    }

    /* 2. Connect. */
    int fd = db_sync_connect(res);
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "[db_sync] connect to %s:%d failed\n", host, port);
        return CWIST_DB_SYNC_ERR_NET;
    }

    unsigned char client_nonce[DB_SYNC_NONCE_LEN];
    unsigned char client_auth[DB_SYNC_MAC_LEN];
    if (RAND_bytes(client_nonce, sizeof(client_nonce)) != 1 ||
        db_sync_mac(ctx, "CWSY client", client_nonce, NULL, NULL, 0, client_auth) != 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }
    if (db_sync_write_all(fd, SYNC_MAGIC, 4, deadline) < 0 ||
        db_sync_write_all(fd, client_nonce, sizeof(client_nonce), deadline) < 0 ||
        db_sync_write_all(fd, client_auth, sizeof(client_auth), deadline) < 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_NET;
    }

    unsigned char server_nonce[DB_SYNC_NONCE_LEN];
    unsigned char server_auth[DB_SYNC_MAC_LEN];
    unsigned char expected[DB_SYNC_MAC_LEN];
    unsigned char len_buf[8];
    if (read_all(fd, server_nonce, sizeof(server_nonce), deadline) < 0 ||
        read_all(fd, server_auth, sizeof(server_auth), deadline) < 0 ||
        db_sync_mac(ctx, "CWSY server", client_nonce, server_nonce, NULL, 0, expected) != 0 ||
        CRYPTO_memcmp(expected, server_auth, DB_SYNC_MAC_LEN) != 0 ||
        read_all(fd, len_buf, sizeof(len_buf), deadline) < 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_PROTO;
    }
    uint64_t blob_len_u64 = 0;
    for (int i = 0; i < 8; i++) blob_len_u64 |= ((uint64_t)len_buf[i]) << (8 * i);
    if (blob_len_u64 == 0) {
        close(fd);
        return CWIST_DB_SYNC_ERR_PROTO;
    }

    if (blob_len_u64 > CWIST_DB_SYNC_MAX_BLOB_SIZE || blob_len_u64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "[db_sync] blob_len %" PRIu64 " exceeds limit\n", blob_len_u64);
        close(fd);
        return CWIST_DB_SYNC_ERR_PROTO;
    }
    size_t blob_len = (size_t)blob_len_u64;
    unsigned char *blob = (unsigned char *)malloc(blob_len);
    if (!blob) {
        close(fd);
        return CWIST_DB_SYNC_ERR_MEM;
    }

    /* 5. Read the snapshot and its challenge-bound authentication tag. */
    unsigned char blob_auth[DB_SYNC_MAC_LEN];
    if (read_all(fd, blob, blob_len, deadline) < 0 ||
        read_all(fd, blob_auth, sizeof(blob_auth), deadline) < 0) {
        free(blob);
        close(fd);
        return CWIST_DB_SYNC_ERR_NET;
    }
    close(fd);
    if (db_sync_mac(ctx, "CWSY blob", client_nonce, server_nonce, blob, blob_len, expected) != 0 ||
        CRYPTO_memcmp(expected, blob_auth, DB_SYNC_MAC_LEN) != 0) {
        free(blob);
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }

    /* 6. Decrypt. */
    size_t pt_len = 0;
    unsigned char *pt = cwist_db_crypt_open(ctx, blob, blob_len, &pt_len);
    free(blob);

    if (!pt) {
        fprintf(stderr, "[db_sync] decrypt failed\n");
        return CWIST_DB_SYNC_ERR_CRYPTO;
    }

    *out_bytes = pt;
    *out_len = pt_len;
    return CWIST_DB_SYNC_OK;
}
