/**
 * @file test_rdbms_auto_mount.c
 * @brief Unit tests for RDBMS auto-detection and runtime mounting.
 */

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cwist/sys/app/app.h>

static void test_null_app(void) {
    bool ok = cwist_app_auto_rdbms(NULL, 5432);
    assert(!ok);
    printf("PASS: null app returns false\n");
}

static void test_unbound_port(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    /* Use a port that is extremely unlikely to be bound */
    bool ok = cwist_app_auto_rdbms(app, 54320);
    assert(!ok);
    assert(app->rdbms == NULL);

    cwist_app_destroy(app);
    printf("PASS: unbound port returns false and leaves rdbms NULL\n");
}

static void test_probe_unbound_port(void) {
    cwist_rdbms_provider_t p = cwist_rdbms_probe_port(54320);
    assert(p == CWIST_RDBMS_NONE);
    printf("PASS: probe on unbound port returns NONE\n");
}

static void test_double_mount(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    /* Manually mount once */
    bool ok = cwist_rdbms_mount_runtime(app, CWIST_RDBMS_POSTGRES, 5432);
    assert(ok);
    assert(app->rdbms != NULL);
    assert(app->rdbms->provider == CWIST_RDBMS_POSTGRES);
    assert(app->rdbms->port == 5432);
    assert(app->rdbms->ready == true);

    /* Second mount should succeed (idempotent) */
    ok = cwist_rdbms_mount_runtime(app, CWIST_RDBMS_MYSQL, 3306);
    assert(ok);
    /* Existing runtime should be left untouched */
    assert(app->rdbms->provider == CWIST_RDBMS_POSTGRES);
    assert(app->rdbms->port == 5432);

    cwist_app_destroy(app);
    printf("PASS: double mount is idempotent\n");
}

static void test_invalid_inputs(void) {
    const int ports[] = {-1, 0, 65536, 100000};
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        assert(cwist_rdbms_probe_port(ports[i]) == CWIST_RDBMS_NONE);

    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(!cwist_rdbms_mount_runtime(app, CWIST_RDBMS_NONE, 5432));
    assert(!cwist_rdbms_mount_runtime(app, (cwist_rdbms_provider_t)99, 5432));
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        assert(!cwist_rdbms_mount_runtime(app, CWIST_RDBMS_POSTGRES, ports[i]));
    assert(app->rdbms == NULL);
    cwist_app_destroy(app);
}

static void test_public_api_declaration(void) {
    /* Ensure the public header declares the function and the struct field */
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    (void)app->rdbms; /* void* in public header */
    cwist_app_destroy(app);
    printf("PASS: public API field accessible\n");
}

#ifdef CWIST_TEST_WRAP
static _Atomic int inject_send_eintr;
static _Atomic int inject_recv_eintr;

ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags) {
    if (atomic_exchange(&inject_send_eintr, 0)) {
        errno = EINTR;
        return -1;
    }
    return __real_send(fd, buf, len, flags);
}

ssize_t __real_recv(int fd, void *buf, size_t len, int flags);
ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags) {
    if (atomic_exchange(&inject_recv_eintr, 0)) {
        errno = EINTR;
        return -1;
    }
    return __real_recv(fd, buf, len, flags);
}
#endif

typedef struct {
    int listen_fd;
    uint16_t port;
    pthread_t thread;
} postgres_mock;

static void *postgres_mock_run(void *arg) {
    postgres_mock *mock = arg;
    int fd = accept(mock->listen_fd, NULL, NULL);
    assert(fd >= 0);
    char startup[64];
    ssize_t n = recv(fd, startup, sizeof(startup), 0);
    assert(n > 0);
    assert(send(fd, "R", 1, MSG_NOSIGNAL) == 1);
    close(fd);
    close(mock->listen_fd);
    return NULL;
}

static void test_interrupted_probe(void) {
    postgres_mock mock = {0};
    mock.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(mock.listen_fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(mock.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(mock.listen_fd, (struct sockaddr *)&addr, &len) == 0);
    mock.port = ntohs(addr.sin_port);
    assert(listen(mock.listen_fd, 1) == 0);
    assert(pthread_create(&mock.thread, NULL, postgres_mock_run, &mock) == 0);

#ifdef CWIST_TEST_WRAP
    atomic_store(&inject_send_eintr, 1);
    atomic_store(&inject_recv_eintr, 1);
#endif
    assert(cwist_rdbms_probe_port(mock.port) == CWIST_RDBMS_POSTGRES);
    assert(pthread_join(mock.thread, NULL) == 0);
}

int main(void) {
    test_interrupted_probe();
    test_null_app();
    test_unbound_port();
    test_probe_unbound_port();
    test_double_mount();
    test_invalid_inputs();
    test_public_api_declaration();
    printf("All RDBMS auto-mount tests passed.\n");
    return 0;
}
