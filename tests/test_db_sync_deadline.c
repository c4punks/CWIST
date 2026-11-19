#define _POSIX_C_SOURCE 200809L
#include "../src/net/db_sync/db_sync.c"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static void *dribble_bytes(void *arg) {
    int fd = *(int *)arg;
    unsigned char bytes[16] = {0};
    for (size_t i = 0; i < sizeof(bytes); i++) {
        ssize_t written = write(fd, bytes + i, 1);
        assert(written == 1);
        struct timespec delay = {.tv_nsec = 25000000L};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

int main(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    pthread_t writer;
    assert(pthread_create(&writer, NULL, dribble_bytes, &fds[1]) == 0);

    unsigned char input[16];
    uint64_t start = db_sync_now_ms();
    int rc = read_all(fds[0], input, sizeof(input), start + 150);
    uint64_t elapsed = db_sync_now_ms() - start;
    assert(rc == -1);
    assert(elapsed >= 100 && elapsed < 1000);

    assert(pthread_join(writer, NULL) == 0);
    close(fds[0]);
    close(fds[1]);
    puts("db_sync absolute deadline stops byte dribbling: OK");
    return 0;
}
