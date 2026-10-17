/**
 * @file test_orm.c
 * @brief Unit tests for the ORM entry points test_orm_socket.c does not
 * cover: raw queries, explicit transactions, INSERT ... RETURNING helpers,
 * the typed select_one helpers and dialect switching. Runs against the
 * in-memory SQLite worker behind cwist_db_transfer_sqlite_to_socket().
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>
#include <cjson/cJSON.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rc(cwist_error_t e) {
    return e.error.err_i16;
}

static cwist_orm_t *open_memdb(void) {
    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    assert(sock >= 0);
    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    assert(orm);
    return orm;
}

static cJSON *row_name_score(const char *name, double score) {
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "name", name);
    cJSON_AddNumberToObject(row, "score", score);
    return row;
}

static void test_query_and_transactions(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(false);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL);")) == 0);

    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(orm, "SELECT 1 AS one, 'x' AS s, NULL AS n, 2.5 AS r;", &rows)) == 0);
    assert(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1);
    cJSON *r0 = cJSON_GetArrayItem(rows, 0);
    assert(cJSON_GetObjectItem(r0, "one")->valuedouble == 1);
    assert(strcmp(cJSON_GetObjectItem(r0, "s")->valuestring, "x") == 0);
    assert(cJSON_IsNull(cJSON_GetObjectItem(r0, "n")));
    assert(cJSON_GetObjectItem(r0, "r")->valuedouble == 2.5);
    cJSON_Delete(rows);

    /* An empty result is an empty array, not NULL. */
    assert(rc(cwist_orm_query(orm, "SELECT * FROM t;", &rows)) == 0);
    assert(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 0);
    cJSON_Delete(rows);

    /* Bad SQL and bad arguments are errors, and the result is reset. */
    rows = (cJSON *)1;
    assert(rc(cwist_orm_query(orm, "SELEKT nonsense;", &rows)) != 0);
    assert(rows == NULL);
    assert(rc(cwist_orm_query(NULL, "SELECT 1;", &rows)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_query(orm, NULL, &rows)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_query(orm, "SELECT 1;", NULL)) == CWIST_ERROR_INVALID_PARAM);

    /* Rolled-back rows disappear, committed rows stay. */
    assert(rc(cwist_orm_exec(orm, "BEGIN;")) == 0);
    cJSON *row = row_name_score("gone", 1);
    assert(rc(cwist_orm_insert(orm, "t", row)) == 0);
    cJSON_Delete(row);
    assert(rc(cwist_orm_rollback(orm)) == 0);
    int count = -1;
    assert(rc(cwist_orm_select_one_int(orm, "t", "c", NULL, &count)) != 0 || count == 0);
    assert(rc(cwist_orm_query(orm, "SELECT COUNT(*) AS c FROM t;", &rows)) == 0);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "c")->valuedouble == 0);
    cJSON_Delete(rows);

    assert(rc(cwist_orm_exec(orm, "BEGIN;")) == 0);
    row = row_name_score("kept", 2);
    assert(rc(cwist_orm_insert(orm, "t", row)) == 0);
    cJSON_Delete(row);
    assert(rc(cwist_orm_commit(orm)) == 0);
    assert(rc(cwist_orm_query(orm, "SELECT name FROM t;", &rows)) == 0);
    assert(cJSON_GetArraySize(rows) == 1);
    assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "name")->valuestring, "kept") == 0);
    cJSON_Delete(rows);

    /* COMMIT / ROLLBACK with no open transaction are SQL errors. */
    assert(rc(cwist_orm_commit(orm)) != 0);
    assert(rc(cwist_orm_rollback(orm)) != 0);
    assert(rc(cwist_orm_commit(NULL)) == CWIST_ERROR_INVALID_PARAM);
    cwist_orm_close_socket(orm);
}

static void test_insert_returning(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE u(id INTEGER PRIMARY KEY, name TEXT, score REAL);")) == 0);

    cJSON *row = row_name_score("a", 1);
    int id_i = 0;
    assert(rc(cwist_orm_insert_returning_int(orm, "u", row, "id", &id_i)) == 0);
    assert(id_i == 1);
    long id_l = 0;
    assert(rc(cwist_orm_insert_returning_long(orm, "u", row, "id", &id_l)) == 0);
    assert(id_l == 2);
    long long id_ll = 0;
    assert(rc(cwist_orm_insert_returning_llong(orm, "u", row, "id", &id_ll)) == 0);
    assert(id_ll == 3);
    cJSON *id_j = NULL;
    assert(rc(cwist_orm_insert_returning_json(orm, "u", row, "id", &id_j)) == 0);
    assert(cJSON_IsNumber(id_j) && id_j->valuedouble == 4);
    cJSON_Delete(id_j);

    /* The _Generic wrapper dispatches on the out type. */
    int via_macro = 0;
    assert(rc(cwist_orm_insert_r(orm, "u", row, "id", &via_macro)) == 0);
    assert(via_macro == 5);

    /* Large rowids round-trip exactly up to 2^53. */
    assert(rc(cwist_orm_exec(orm, "INSERT INTO u(id, name) VALUES (9007199254740990, 'big');")) == 0);
    assert(rc(cwist_orm_insert_returning_llong(orm, "u", row, "id", &id_ll)) == 0);
    assert(id_ll == 9007199254740991LL);
    cJSON_Delete(row);

    /* Invalid arguments and failed inserts. */
    row = row_name_score("b", 2);
    assert(rc(cwist_orm_insert_returning_int(NULL, "u", row, "id", &id_i)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, NULL, row, "id", &id_i)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, "u", NULL, "id", &id_i)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, "u", row, NULL, &id_i)) == CWIST_ERROR_INVALID_PARAM);
    id_j = (cJSON *)1;
    assert(rc(cwist_orm_insert_returning_json(orm, "u", row, "id", NULL)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_json(orm, "missing_table", row, "id", &id_j)) != 0);
    assert(id_j == NULL);
    cJSON_Delete(row);
    cwist_orm_close_socket(orm);
}

static void test_select_one(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE s(id INTEGER PRIMARY KEY, name TEXT, score REAL, n INTEGER);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO s VALUES (1, 'alice', 1.5, 7), (2, 'bob', -2.25, NULL);")) == 0);

    int i = 0;
    long l = 0;
    long long ll = 0;
    double d = 0;
    char *str = NULL;
    assert(rc(cwist_orm_select_one_int(orm, "s", "n", "id = 1", &i)) == 0 && i == 7);
    assert(rc(cwist_orm_select_one_long(orm, "s", "n", "id = 1", &l)) == 0 && l == 7);
    assert(rc(cwist_orm_select_one_llong(orm, "s", "id", "name = 'bob'", &ll)) == 0 && ll == 2);
    assert(rc(cwist_orm_select_one_double(orm, "s", "score", "id = 2", &d)) == 0 && d == -2.25);
    assert(rc(cwist_orm_select_one_string(orm, "s", "name", "id = 2", &str)) == 0);
    assert(strcmp(str, "bob") == 0);
    free(str);
    str = NULL;

    /* No WHERE: first row. */
    assert(rc(cwist_orm_select_one_string(orm, "s", "name", NULL, &str)) == 0 && str);
    free(str);
    str = NULL;

    /* _Generic wrapper. */
    double via_macro = 0;
    assert(rc(cwist_orm_select_one(orm, "s", "score", "id = 1", &via_macro)) == 0 && via_macro == 1.5);

    /* Type mismatch, NULL value and no matching row are errors. */
    assert(rc(cwist_orm_select_one_int(orm, "s", "name", "id = 1", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_string(orm, "s", "score", "id = 1", &str)) == CWIST_ERROR_PROTOCOL);
    assert(str == NULL);
    assert(rc(cwist_orm_select_one_int(orm, "s", "n", "id = 2", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_int(orm, "s", "n", "id = 99", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_int(orm, "nope", "n", NULL, &i)) != 0);
    cJSON *j = (cJSON *)1;
    assert(rc(cwist_orm_select_one_json(orm, NULL, "n", NULL, &j)) == CWIST_ERROR_INVALID_PARAM);
    assert(j == NULL);
    cwist_orm_close_socket(orm);
}

/* Values the worker used to corrupt: integers from ~1e15 up were printed
 * with 15 significant digits, and text such as "007" became the number 7. */
static void test_value_fidelity(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE f(id INTEGER PRIMARY KEY, code TEXT);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO f VALUES (1234567890123456, '007'), (2, ' 12'), (3, '-5');")) == 0);
    long long ll = 0;
    assert(rc(cwist_orm_select_one_llong(orm, "f", "id", "code = '007'", &ll)) == 0);
    assert(ll == 1234567890123456LL);
    char *str = NULL;
    assert(rc(cwist_orm_select_one_string(orm, "f", "code", "id = 1234567890123456", &str)) == 0);
    assert(strcmp(str, "007") == 0);
    free(str);
    str = NULL;
    assert(rc(cwist_orm_select_one_string(orm, "f", "code", "id = 2", &str)) == 0);
    assert(strcmp(str, " 12") == 0);
    free(str);
    /* Canonical integer text still comes back as a number (known limit of
     * the text-based worker protocol). */
    int i = 0;
    assert(rc(cwist_orm_select_one_int(orm, "f", "code", "id = 3", &i)) == 0 && i == -5);
    cwist_orm_close_socket(orm);
}

/* #354: the exact path keeps integers above 2^53 that the double-based
 * helpers round. */
static void test_exact_i64(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE x(id INTEGER PRIMARY KEY, v, t TEXT);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO x VALUES "
                                  "(1, 9007199254740993, '9223372036854775807'), "
                                  "(2, -9223372036854775808, '007'), "
                                  "(3, 1.5, NULL), (4, 0, '-0');")) == 0);
    int64_t v = 7;
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 1", &v)) == 0);
    assert(v == INT64_C(9007199254740993));
    long long lossy = 0;
    assert(rc(cwist_orm_select_one_llong(orm, "x", "v", "id = 1", &lossy)) == 0);
    assert(lossy != 9007199254740993LL); /* why the exact path exists */
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 2", &v)) == 0 && v == INT64_MIN);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 4", &v)) == 0 && v == 0);

    /* A TEXT cell holding canonical integer text, as other workers may send. */
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 1", &v)) == 0 && v == INT64_MAX);

    /* Not an exact integer: out is left alone. */
    v = 42;
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 3", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 2", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 3", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 4", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 99", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "missing", "v", NULL, &v)) == CWIST_ERROR_PROTOCOL);
    assert(v == 42);
    assert(rc(cwist_orm_select_one_i64(NULL, "x", "v", NULL, &v)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", NULL, NULL)) == CWIST_ERROR_INVALID_PARAM);

    /* Rowids above 2^53 come back exactly from INSERT ... RETURNING. */
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE big(id INTEGER PRIMARY KEY, name TEXT);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO big VALUES (1152921504606846975, 'seed');")) == 0);
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "name", "next");
    int64_t id = 0;
    assert(rc(cwist_orm_insert_returning_i64(orm, "big", row, "id", &id)) == 0);
    assert(id == INT64_C(1152921504606846976));
    cwist_orm_use_dialect(orm, CWIST_ORM_POSTGRES); /* RETURNING form */
    assert(rc(cwist_orm_insert_returning_i64(orm, "big", row, "id", &id)) == 0);
    assert(id == INT64_C(1152921504606846977));
    cwist_orm_use_dialect(orm, CWIST_ORM_SQLITE);
    assert(rc(cwist_orm_insert_returning_i64(orm, "big", NULL, "id", &id)) ==
           CWIST_ERROR_INVALID_PARAM);
    cJSON_Delete(row);
    cwist_orm_close_socket(orm);
}

static void test_dialects(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE d(id INTEGER PRIMARY KEY, name TEXT);")) == 0);
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "name", "q");

    /* Postgres dialect: RETURNING clause, which SQLite >= 3.35 accepts too. */
    cwist_orm_use_dialect(orm, CWIST_ORM_POSTGRES);
    int id = 0;
    assert(rc(cwist_orm_insert_returning_int(orm, "d", row, "id", &id)) == 0 && id == 1);

    /* MySQL dialect: LAST_INSERT_ID() does not exist in SQLite, so the
     * INSERT runs (row 2) and the id query fails; the error must surface
     * instead of a bogus id. */
    cwist_orm_use_dialect(orm, CWIST_ORM_MYSQL);
    id = 0;
    assert(rc(cwist_orm_insert_returning_int(orm, "d", row, "id", &id)) != 0);

    cwist_orm_use_dialect(orm, CWIST_ORM_SQLITE);
    assert(rc(cwist_orm_insert_returning_int(orm, "d", row, "id", &id)) == 0 && id == 3);
    cwist_orm_use_dialect(NULL, CWIST_ORM_POSTGRES); /* ignored */
    cJSON_Delete(row);
    cwist_orm_close_socket(orm);
}

/* #354 ask 2: values typed by the column's SQLite storage class, not by
 * their text.  TEXT holding digits stays TEXT, INTEGER stays exact above
 * 2^53, BLOB comes back as bytes with a length, and NULL is distinct from
 * an empty string. */
static void test_query_typed(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm,
                             "CREATE TABLE q(id INTEGER PRIMARY KEY, r REAL, s TEXT, b BLOB, n TEXT);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO q VALUES "
                                  "(1, 2.5, 'hello', x'00ff10', NULL), "
                                  "(9007199254740993, -0.25, '007', x'', '');")) == 0);

    cwist_orm_result_t *res = NULL;
    assert(rc(cwist_orm_query_typed(orm, "SELECT id, r, s, b, n FROM q ORDER BY r DESC;", &res)) == 0);
    assert(res && res->row_count == 2 && res->column_count == 5);
    assert(cwist_orm_result_column_index(res, "id") == 0);
    assert(cwist_orm_result_column_index(res, "n") == 4);
    assert(cwist_orm_result_column_index(res, "missing") == res->column_count);
    assert(cwist_orm_result_column_index(NULL, "id") == 0);
    assert(cwist_orm_result_cell(NULL, 0, 0) == NULL);
    assert(cwist_orm_result_cell(res, 2, 0) == NULL);
    assert(cwist_orm_result_cell(res, 0, 5) == NULL);

    /* Mixed-type first row: one of every storage class. */
    const cwist_orm_value_t *cell = cwist_orm_result_cell(res, 0, 0);
    assert(cell && cell->type == CWIST_ORM_TYPE_INTEGER && cell->i64 == 1);
    cell = cwist_orm_result_cell(res, 0, 1);
    assert(cell && cell->type == CWIST_ORM_TYPE_REAL && cell->real == 2.5);
    cell = cwist_orm_result_cell(res, 0, 2);
    assert(cell && cell->type == CWIST_ORM_TYPE_TEXT && strcmp(cell->text, "hello") == 0);
    cell = cwist_orm_result_cell(res, 0, 3);
    assert(cell && cell->type == CWIST_ORM_TYPE_BLOB && cell->blob_len == 3);
    assert(cell->blob[0] == 0x00 && cell->blob[1] == 0xff && cell->blob[2] == 0x10);
    cell = cwist_orm_result_cell(res, 0, 4);
    assert(cell && cell->type == CWIST_ORM_TYPE_NULL && cell->text == NULL && cell->blob == NULL);

    /* Second row: integer above 2^53 stays exact, canonical-integer TEXT
     * stays TEXT, empty BLOB has length 0, empty TEXT is not NULL. */
    cell = cwist_orm_result_cell(res, 1, 0);
    assert(cell && cell->type == CWIST_ORM_TYPE_INTEGER && cell->i64 == INT64_C(9007199254740993));
    cell = cwist_orm_result_cell(res, 1, 1);
    assert(cell && cell->type == CWIST_ORM_TYPE_REAL && cell->real == -0.25);
    cell = cwist_orm_result_cell(res, 1, 2);
    assert(cell && cell->type == CWIST_ORM_TYPE_TEXT && strcmp(cell->text, "007") == 0);
    cell = cwist_orm_result_cell(res, 1, 3);
    assert(cell && cell->type == CWIST_ORM_TYPE_BLOB && cell->blob_len == 0 && cell->blob == NULL);
    cell = cwist_orm_result_cell(res, 1, 4);
    assert(cell && cell->type == CWIST_ORM_TYPE_TEXT && cell->text[0] == '\0');
    cwist_orm_result_free(res);
    res = NULL;

    /* Zero rows still reports the columns. */
    assert(rc(cwist_orm_query_typed(orm, "SELECT id, s FROM q WHERE id = 99;", &res)) == 0);
    assert(res && res->row_count == 0 && res->column_count == 2);
    assert(cwist_orm_result_cell(res, 0, 0) == NULL);
    cwist_orm_result_free(res);
    res = NULL;

    /* Errors reset the result. */
    assert(rc(cwist_orm_query_typed(orm, "SELEKT nonsense;", &res)) != 0 && res == NULL);
    assert(rc(cwist_orm_query_typed(NULL, "SELECT 1;", &res)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_query_typed(orm, NULL, &res)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_query_typed(orm, "SELECT 1;", NULL)) == CWIST_ERROR_INVALID_PARAM);

    /* BLOB bytes that are not valid UTF-8 and do not survive as text. */
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE raw(b BLOB);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO raw VALUES (x'c0ffee42');")) == 0);
    assert(rc(cwist_orm_query_typed(orm, "SELECT b FROM raw;", &res)) == 0);
    cell = cwist_orm_result_cell(res, 0, 0);
    assert(cell && cell->type == CWIST_ORM_TYPE_BLOB && cell->blob_len == 4);
    assert(cell->blob[0] == 0xc0 && cell->blob[1] == 0xff && cell->blob[2] == 0xee &&
           cell->blob[3] == 0x42);
    cwist_orm_result_free(res);
    cwist_orm_result_free(NULL);

    /* The legacy path still types by text: the id is a JSON number rounded
     * to a double, and '007' is a string (not canonical integer text). */
    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(orm, "SELECT id, s FROM q WHERE id = 9007199254740993;", &rows)) == 0);
    cJSON *legacy_row = cJSON_GetArrayItem(rows, 0);
    cJSON *legacy_id = cJSON_GetObjectItem(legacy_row, "id");
    assert(cJSON_IsNumber(legacy_id) && legacy_id->valuedouble == 9007199254740992.0);
    assert(cJSON_IsString(cJSON_GetObjectItem(legacy_row, "s")));
    cJSON_Delete(rows);

    cwist_orm_close_socket(orm);
}

int main(void) {
    test_query_and_transactions();
    test_insert_returning();
    test_select_one();
    test_value_fidelity();
    test_exact_i64();
    test_query_typed();
    test_dialects();
    cwist_orm_immediate_commit(false);
    printf("test_orm: ok\n");
    return 0;
}
