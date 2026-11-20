#include <assert.h>
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_missing_user_lookup(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "SELECT username FROM users WHERE id = -1;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL);
    assert(cJSON_GetArraySize(result) == 0);
    cJSON_Delete(result);
}

static void test_missing_table_query(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "SELECT * FROM __cwist_missing_table;", &result);
    assert(err.errtype == CWIST_ERR_JSON);
    assert(result == NULL);
    cwist_error_dispose(&err);
}

static void test_null_db_guards(void) {
    cJSON *result = NULL;
    cwist_error_t query_err = cwist_db_query(NULL, "SELECT 1;", &result);
    assert(query_err.error.err_i16 == -1);
    assert(result == NULL);

    cwist_error_t exec_err = cwist_db_exec(NULL, "CREATE TABLE x(id INT);");
    assert(exec_err.error.err_i16 == -1);
}

static void test_integrity_check(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "PRAGMA integrity_check;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL);
    assert(cJSON_GetArraySize(result) == 1);
    cJSON *row = cJSON_GetArrayItem(result, 0);
    assert(row != NULL);
    cJSON *val = cJSON_GetObjectItem(row, "integrity_check");
    assert(val && val->valuestring);
    assert(strcmp(val->valuestring, "ok") == 0);
    cJSON_Delete(result);
}

int main(void) {
    const char *db_path = "/tmp/nuke_test.db";

    sqlite3 *setup = NULL;
    if (sqlite3_open(db_path, &setup) == SQLITE_OK) {
        sqlite3_exec(setup, "CREATE TABLE IF NOT EXISTS users(id INT, username TEXT);", NULL, NULL,
                     NULL);
        sqlite3_close(setup);
    } else {
        fprintf(stderr, "Failed to create test DB\n");
        return 1;
    }

    if (cwist_nuke_init(db_path, 0) != 0) {
        fprintf(stderr, "cwist_nuke_init failed\n");
        unlink(db_path);
        return 1;
    }

    sqlite3 *handle = cwist_nuke_get_db();
    if (!handle) {
        fprintf(stderr, "cwist_nuke_get_db returned NULL\n");
        cwist_nuke_close();
        unlink(db_path);
        return 1;
    }

    cwist_db db = {0};
    db.conn = handle;

    test_missing_user_lookup(&db);
    test_missing_table_query(&db);
    test_integrity_check(&db);
    test_null_db_guards();

    assert(sqlite3_exec(handle, "CREATE TABLE IF NOT EXISTS stable_state(id INT);", NULL, NULL,
                        NULL) == SQLITE_OK);
    assert(cwist_nuke_force_disk_mode() == 0);
    assert(cwist_nuke_get_db() == handle);
    assert(sqlite3_exec(handle, "INSERT INTO stable_state VALUES (1);", NULL, NULL, NULL) ==
           SQLITE_OK);
    assert(sqlite3_exec(handle, "INSERT INTO stable_state VALUES (2);", NULL, NULL, NULL) ==
           SQLITE_OK);

    printf("[NukeDB] Missing-record safety tests passed.\n");

    cwist_nuke_close();
    setup = NULL;
    assert(sqlite3_open(db_path, &setup) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(setup, "SELECT count(*) FROM stable_state;", -1, &stmt, NULL) ==
           SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);
    sqlite3_close(setup);
    unlink(db_path);
    return 0;
}
