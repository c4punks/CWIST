#define _POSIX_C_SOURCE 200809L
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int listener;
    atomic_int received;
} server_ctx;

typedef struct {
    cwist_redis_t *redis;
    atomic_int done;
    int result;
} reader_ctx;

static void *stall_server(void *arg) {
    server_ctx *server = arg;
    int fd = accept(server->listener, NULL, NULL);
    assert(fd >= 0);
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    assert(n > 0);
    atomic_store(&server->received, 1);
    n = read(fd, buf, sizeof(buf));
    assert(n <= 0);
    close(fd);
    close(server->listener);
    return NULL;
}

static void *blocked_reader(void *arg) {
    reader_ctx *reader = arg;
    const void *argv[] = {"PING"};
    size_t lens[] = {4};
    char *out = NULL;
    size_t out_len = 0;
    cwist_error_t err = cwist_redis_command_argv(reader->redis, 1, argv, lens, &out, &out_len);
    reader->result = err.error.err_i16;
    cwist_error_dispose(&err);
    cwist_free(out);
    atomic_store(&reader->done, 1);
    return NULL;
}

int main(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &addr_len) == 0);

    server_ctx server = {.listener = listener};
    atomic_init(&server.received, 0);
    pthread_t server_thread;
    assert(pthread_create(&server_thread, NULL, stall_server, &server) == 0);

    reader_ctx reader = {.redis = cwist_redis_connect("127.0.0.1", ntohs(addr.sin_port))};
    assert(reader.redis != NULL);
    assert(cwist_redis_set_timeout_ms(reader.redis, 10000) == 0);
    atomic_init(&reader.done, 0);
    pthread_t reader_thread;
    assert(pthread_create(&reader_thread, NULL, blocked_reader, &reader) == 0);

    for (int i = 0; i < 200 && !atomic_load(&server.received); i++) usleep(10000);
    assert(atomic_load(&server.received));
    cwist_redis_close(reader.redis);

    assert(pthread_join(reader_thread, NULL) == 0);
    assert(pthread_join(server_thread, NULL) == 0);
    assert(atomic_load(&reader.done));
    assert(reader.result != 0);
    puts("Redis close synchronized with active reader: OK");
    return 0;
}
