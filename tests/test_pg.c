#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pg.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#if defined(CWIST_HAS_LIBPQ)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

static void test_silent_peer_times_out(void) {
#if defined(CWIST_HAS_LIBPQ)
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
    snprintf(conninfo, sizeof(conninfo), "host=127.0.0.1 port=%u dbname=test",
             (unsigned int)ntohs(addr.sin_port));
    time_t started = time(NULL);
    cwist_pg_t *pg = cwist_pg_connect(conninfo);
    time_t elapsed = time(NULL) - started;
    assert(pg == NULL);
    assert(elapsed >= 4 && elapsed <= 8);
    close(listener);
#endif
}

int main(void) {
    /* Invalid conninfo / NULL handling */
    assert(cwist_pg_connect(NULL) == NULL);
    assert(cwist_pg_connect("") == NULL);

    /* Connection to unreachable dummy port should fail gracefully without crashing */
    cwist_pg_t *pg = cwist_pg_connect("host=127.0.0.1 port=65432 dbname=nonexistent connect_timeout=1");
    assert(pg == NULL);
    test_silent_peer_times_out();

    /* Disconnect NULL is safe */
    cwist_pg_disconnect(NULL);
    assert(!cwist_pg_is_connected(NULL));

    /* NULL pg handle for query operations should return invalid param */
    cwist_error_t err = cwist_pg_exec(NULL, "SELECT 1;");
    assert(!cwist_error_is_ok(&err));

    cJSON *rows = NULL;
    err = cwist_pg_exec_params(NULL, "SELECT $1::text;", 0, NULL, &rows);
    assert(!cwist_error_is_ok(&err));
    assert(rows == NULL);
#if defined(CWIST_HAS_LIBPQ)
    err = cwist_pg_exec_params(NULL, "SELECT $1::text;", -1, NULL, &rows);
    assert(err.error.err_i16 == CWIST_ERROR_INVALID_PARAM && rows == NULL);
    err = cwist_pg_exec_params(NULL, "SELECT $1::text;", 1, NULL, &rows);
    assert(err.error.err_i16 == CWIST_ERROR_INVALID_PARAM && rows == NULL);
#endif

    err = cwist_pg_begin(NULL);
    assert(!cwist_error_is_ok(&err));

    err = cwist_pg_commit(NULL);
    assert(!cwist_error_is_ok(&err));

    err = cwist_pg_rollback(NULL);
    assert(!cwist_error_is_ok(&err));

    assert(cwist_pg_last_error(NULL) != NULL);

    puts("test_pg: all passed");
    return 0;
}
