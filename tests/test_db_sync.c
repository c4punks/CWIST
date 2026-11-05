#define _POSIX_C_SOURCE 200809L
#include <cwist/net/db_sync/db_sync.h>
#include <cwist/security/db_crypt/db_crypt.h>
#include <sqlite3.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
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

static void test_oversized_blob_is_rejected_before_allocation(void) {
    int port = 0;
    int listener = listener_on_loopback(&port);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int fd = accept(listener, NULL, NULL);
        unsigned char magic[4];
        unsigned char length[8];
        ssize_t ignored = read(fd, magic, sizeof(magic));
        (void)ignored;
        memset(length, 0xff, sizeof(length));
        ignored = write(fd, length, sizeof(length));
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
    const unsigned char magic[4] = {0x43, 0x57, 0x53, 0x59};
    assert(write(fd, magic, sizeof(magic)) == (ssize_t)sizeof(magic));
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    test_oversized_blob_is_rejected_before_allocation();
    test_reset_peer_does_not_raise_sigpipe();
    puts("test_db_sync: OK");
    return 0;
}
