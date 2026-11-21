#define _POSIX_C_SOURCE 200809L
#include <cwist/net/db_sync/db_sync.h>
#include <cwist/security/db_crypt/db_crypt.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int listener_on_loopback(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    assert(listen(fd, 1) == 0);
    *port = ntohs(addr.sin_port);
    return fd;
}

static void sync_mac(const cwist_db_crypt_ctx_t *ctx, const char *domain,
                     const unsigned char client_nonce[32], const unsigned char server_nonce[32],
                     unsigned char out[32]) {
    static const unsigned char zero[32];
    unsigned int out_len = 0;
    HMAC_CTX *mac = HMAC_CTX_new();
    assert(mac != NULL);
    assert(HMAC_Init_ex(mac, ctx->kek, sizeof(ctx->kek), EVP_sha256(), NULL) == 1);
    assert(HMAC_Update(mac, (const unsigned char *)domain, strlen(domain)) == 1);
    assert(HMAC_Update(mac, client_nonce, 32) == 1);
    assert(HMAC_Update(mac, server_nonce ? server_nonce : zero, 32) == 1);
    assert(HMAC_Final(mac, out, &out_len) == 1 && out_len == 32);
    HMAC_CTX_free(mac);
}

static int connect_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((uint16_t)port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    for (int i = 0; i < 200; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        struct timespec delay = {0, 10000000L};
        nanosleep(&delay, NULL);
    }
    close(fd);
    return -1;
}

static void test_oversized_blob_is_rejected_before_allocation(void) {
    int port = 0;
    int listener = listener_on_loopback(&port);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int fd = accept(listener, NULL, NULL);
        unsigned char hello[68];
        size_t hello_off = 0;
        while (hello_off < sizeof(hello)) {
            ssize_t n = read(fd, hello + hello_off, sizeof(hello) - hello_off);
            assert(n > 0);
            hello_off += (size_t)n;
        }
        cwist_db_crypt_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        unsigned char nonce[32] = {0};
        unsigned char auth[32];
        sync_mac(&ctx, "CWSY server", hello + 4, nonce, auth);
        unsigned char length[8];
        memset(length, 0xff, sizeof(length));
        assert(write(fd, nonce, sizeof(nonce)) == (ssize_t)sizeof(nonce));
        assert(write(fd, auth, sizeof(auth)) == (ssize_t)sizeof(auth));
        assert(write(fd, length, sizeof(length)) == (ssize_t)sizeof(length));
        close(fd);
        close(listener);
        _exit(0);
    }
    close(listener);

    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = cwist_db_sync_pull("127.0.0.1", port, &ctx, &out, &out_len);
    assert(rc == CWIST_DB_SYNC_ERR_PROTO);
    assert(out == NULL && out_len == 0);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_reset_peer_does_not_raise_sigpipe(void) {
    int port = 0;
    int listener = listener_on_loopback(&port);
    close(listener);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY);", NULL, NULL, NULL);
        cwist_db_crypt_ctx_t ctx;
        memset(&ctx, 0x42, sizeof(ctx));
        int rc = cwist_db_sync_serve(db, &ctx, port);
        sqlite3_close(db);
        _exit(rc == CWIST_DB_SYNC_ERR_NET ? 0 : 1);
    }

    int fd = -1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((uint16_t)port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    for (int i = 0; i < 200; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
        close(fd);
        fd = -1;
        struct timespec delay = {0, 10000000L};
        nanosleep(&delay, NULL);
    }
    assert(fd >= 0);
    unsigned char hello[68] = {0x43, 0x57, 0x53, 0x59};
    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0x42, sizeof(ctx));
    sync_mac(&ctx, "CWSY client", hello + 4, NULL, hello + 36);
    assert(write(fd, hello, sizeof(hello)) == (ssize_t)sizeof(hello));
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_authenticated_sync_survives_unauthenticated_client(void) {
    int port = 0;
    int listener = listener_on_loopback(&port);
    close(listener);
    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0x24, sizeof(ctx));
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        sqlite3 *db = NULL;
        assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY);", NULL, NULL, NULL) ==
               SQLITE_OK);
        int rc = cwist_db_sync_serve(db, &ctx, port);
        sqlite3_close(db);
        _exit(rc == CWIST_DB_SYNC_OK ? 0 : 1);
    }

    int bad = connect_loopback(port);
    assert(bad >= 0);
    const unsigned char magic[4] = {0x43, 0x57, 0x53, 0x59};
    assert(write(bad, magic, sizeof(magic)) == (ssize_t)sizeof(magic));
    close(bad);

    unsigned char *out = NULL;
    size_t out_len = 0;
    assert(cwist_db_sync_pull("127.0.0.1", port, &ctx, &out, &out_len) == CWIST_DB_SYNC_OK);
    assert(out_len >= 16 && memcmp(out, "SQLite format 3", 15) == 0);
    free(out);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* A client that connects and never sends its hello must not hold the serve
 * window: after the per-client hello timeout the server accepts the next
 * client, so the real replica still syncs well before the 30 s deadline. */
static void test_silent_client_does_not_block_sync(void) {
    int port = 0;
    int listener = listener_on_loopback(&port);
    close(listener);
    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0x31, sizeof(ctx));
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        sqlite3 *db = NULL;
        assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
        int rc = cwist_db_sync_serve(db, &ctx, port);
        sqlite3_close(db);
        _exit(rc == CWIST_DB_SYNC_OK ? 0 : 1);
    }

    int silent = connect_loopback(port);
    assert(silent >= 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned char *out = NULL;
    size_t out_len = 0;
    assert(cwist_db_sync_pull("127.0.0.1", port, &ctx, &out, &out_len) == CWIST_DB_SYNC_OK);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(out);
    assert(t1.tv_sec - t0.tv_sec < 15);
    close(silent);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    test_oversized_blob_is_rejected_before_allocation();
    test_silent_client_does_not_block_sync();
    test_reset_peer_does_not_raise_sigpipe();
    test_authenticated_sync_survives_unauthenticated_client();
    puts("test_db_sync: OK");
    return 0;
}
