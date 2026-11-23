#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/mysql.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    /* Invalid conninfo / NULL handling */
    assert(cwist_mysql_connect(NULL) == NULL);
    assert(cwist_mysql_connect("") == NULL);

    /* Connection to unreachable dummy port should fail gracefully without crashing */
    cwist_mysql_t *mysql = cwist_mysql_connect("host=127.0.0.1 port=65432 user=root dbname=nonexistent");
    assert(mysql == NULL);

    /* URI format to unreachable dummy port should also fail gracefully */
    mysql = cwist_mysql_connect("mysql://root:secret@127.0.0.1:65432/nonexistent");
    assert(mysql == NULL);

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
