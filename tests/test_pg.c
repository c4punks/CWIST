#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pg.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    /* Invalid conninfo / NULL handling */
    assert(cwist_pg_connect(NULL) == NULL);
    assert(cwist_pg_connect("") == NULL);

    /* Connection to unreachable dummy port should fail gracefully without crashing */
    cwist_pg_t *pg = cwist_pg_connect("host=127.0.0.1 port=65432 dbname=nonexistent connect_timeout=1");
    assert(pg == NULL);

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
