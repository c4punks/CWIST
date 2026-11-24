#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/mysql.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#if defined(CWIST_HAS_MYSQL)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

static void test_silent_peer_times_out(void) {
#if defined(CWIST_HAS_MYSQL)
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &addr_len) == 0);
    assert(listen(listener, 1) == 0);

    char conninfo[256];
    snprintf(conninfo, sizeof(conninfo), "host=127.0.0.1 port=%u user=root dbname=test",
             (unsigned int)ntohs(addr.sin_port));
    time_t started = time(NULL);
    cwist_mysql_t *mysql = cwist_mysql_connect(conninfo);
    time_t elapsed = time(NULL) - started;
    assert(mysql == NULL);
    assert(elapsed >= 4 && elapsed <= 8);
    close(listener);
#endif
}

int main(void) {
    /* Invalid conninfo / NULL handling */
    assert(cwist_mysql_connect(NULL) == NULL);
    assert(cwist_mysql_connect("") == NULL);

    /* Connection to unreachable dummy port should fail gracefully without crashing */
    cwist_mysql_t *mysql =
        cwist_mysql_connect("host=127.0.0.1 port=65432 user=root dbname=nonexistent");
    assert(mysql == NULL);

    /* URI format to unreachable dummy port should also fail gracefully */
    mysql = cwist_mysql_connect("mysql://root:secret@127.0.0.1:65432/nonexistent");
    assert(mysql == NULL);

    const char *invalid[] = {
        "password=\"secret",         "password='secret",        "password=\"secret\"junk",
        "password=secret junk",      "host=127.0.0.1 port=abc", "host=127.0.0.1 port=0",
        "host=127.0.0.1 port=65536",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(cwist_mysql_connect(invalid[i]) == NULL);
    test_silent_peer_times_out();

    /* Disconnect NULL is safe */
    cwist_mysql_disconnect(NULL);
    assert(!cwist_mysql_is_connected(NULL));

    /* NULL mysql handle for query operations should return invalid param */
    cwist_error_t err = cwist_mysql_exec(NULL, "SELECT 1;");
    assert(!cwist_error_is_ok(&err));

    cJSON *rows = NULL;
    err = cwist_mysql_exec_params(NULL, "SELECT ?;", 0, NULL, &rows);
    assert(!cwist_error_is_ok(&err));
    assert(rows == NULL);
#if defined(CWIST_HAS_MYSQL)
    err = cwist_mysql_exec_params(NULL, "SELECT ?;", -1, NULL, &rows);
    assert(err.error.err_i16 == CWIST_ERROR_INVALID_PARAM && rows == NULL);
    err = cwist_mysql_exec_params(NULL, "SELECT ?;", 1, NULL, &rows);
    assert(err.error.err_i16 == CWIST_ERROR_INVALID_PARAM && rows == NULL);
#endif

    err = cwist_mysql_begin(NULL);
    assert(!cwist_error_is_ok(&err));

    err = cwist_mysql_commit(NULL);
    assert(!cwist_error_is_ok(&err));

    err = cwist_mysql_rollback(NULL);
    assert(!cwist_error_is_ok(&err));

    assert(cwist_mysql_last_error(NULL) != NULL);

    puts("test_mysql: all passed");
    return 0;
}
