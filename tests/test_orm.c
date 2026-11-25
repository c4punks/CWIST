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
#include <cwist/core/db/sql.h>
#include <cwist/sys/app/app.h>
#include <cjson/cJSON.h>
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int rc(cwist_error_t e) {
    return e.error.err_i16;
}

static _Atomic int inject_recv_eintr;
ssize_t __real_recv(int fd, void *buf, size_t len, int flags);
ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags) {
    if (atomic_exchange(&inject_recv_eintr, 0)) {
        errno = EINTR;
        return -1;
    }
    return __real_recv(fd, buf, len, flags);
}

static int fail_cwist_alloc;
void *__real_cwist_alloc(size_t size);
void *__wrap_cwist_alloc(size_t size) {
    if (fail_cwist_alloc) return NULL;
    return __real_cwist_alloc(size);
}

static int fail_json_print;
char *__real_cJSON_PrintUnformatted(const cJSON *item);
char *__wrap_cJSON_PrintUnformatted(const cJSON *item) {
    if (fail_json_print) return NULL;
    return __real_cJSON_PrintUnformatted(item);
}

static cwist_orm_t *open_memdb(void) {
    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    assert(sock >= 0);
    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    assert(orm);
    return orm;
}

static char shared_db_path[160];
static int shared_db_generation;

static void next_shared_db(void) {
    snprintf(shared_db_path, sizeof(shared_db_path), "/tmp/cwist-orm-test-%d-%d.db", getpid(),
             shared_db_generation++);
}

static cwist_orm_t *open_shared_memdb(void) {
    if (!shared_db_path[0]) next_shared_db();
    int sock = cwist_db_transfer_sqlite_to_socket(shared_db_path);
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
    assert(rc(cwist_orm_exec(
               orm, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL);")) == 0);

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
    assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "name")->valuestring, "kept") ==
           0);
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
    assert(rc(cwist_orm_exec(
               orm, "CREATE TABLE u(id INTEGER PRIMARY KEY, name TEXT, score REAL);")) == 0);

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
    assert(rc(cwist_orm_exec(orm, "INSERT INTO u(id, name) VALUES (9007199254740990, 'big');")) ==
           0);
    assert(rc(cwist_orm_insert_returning_llong(orm, "u", row, "id", &id_ll)) == 0);
    assert(id_ll == 9007199254740991LL);
    cJSON_Delete(row);

    /* Invalid arguments and failed inserts. */
    row = row_name_score("b", 2);
    assert(rc(cwist_orm_insert_returning_int(NULL, "u", row, "id", &id_i)) ==
           CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, NULL, row, "id", &id_i)) ==
           CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, "u", NULL, "id", &id_i)) ==
           CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_int(orm, "u", row, NULL, &id_i)) ==
           CWIST_ERROR_INVALID_PARAM);
    id_j = (cJSON *)1;
    assert(rc(cwist_orm_insert_returning_json(orm, "u", row, "id", NULL)) ==
           CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_insert_returning_json(orm, "missing_table", row, "id", &id_j)) != 0);
    assert(id_j == NULL);
    cJSON_Delete(row);
    cwist_orm_close_socket(orm);
}

static void test_select_one(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(
               orm, "CREATE TABLE s(id INTEGER PRIMARY KEY, name TEXT, score REAL, n INTEGER);")) ==
           0);
    assert(rc(cwist_orm_exec(
               orm, "INSERT INTO s VALUES (1, 'alice', 1.5, 7), (2, 'bob', -2.25, NULL);")) == 0);

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
    assert(rc(cwist_orm_select_one(orm, "s", "score", "id = 1", &via_macro)) == 0 &&
           via_macro == 1.5);

    /* Type mismatch, NULL value and no matching row are errors. */
    assert(rc(cwist_orm_select_one_int(orm, "s", "name", "id = 1", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_string(orm, "s", "score", "id = 1", &str)) ==
           CWIST_ERROR_PROTOCOL);
    assert(str == NULL);
    assert(rc(cwist_orm_select_one_int(orm, "s", "n", "id = 2", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_int(orm, "s", "n", "id = 99", &i)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_int(orm, "nope", "n", NULL, &i)) != 0);
    cJSON *j = (cJSON *)1;
    assert(rc(cwist_orm_select_one_json(orm, NULL, "n", NULL, &j)) == CWIST_ERROR_INVALID_PARAM);
    assert(j == NULL);
    assert(rc(cwist_orm_select(orm, "s", "n", NULL, NULL)) == CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_select_one_double(orm, "s", "score", NULL, NULL)) ==
           CWIST_ERROR_INVALID_PARAM);
    assert(rc(cwist_orm_select_one_string(orm, "s", "name", NULL, NULL)) ==
           CWIST_ERROR_INVALID_PARAM);
    cwist_orm_close_socket(orm);
}

/* Values the worker used to corrupt: integers from ~1e15 up were printed
 * with 15 significant digits, and text such as "007" became the number 7. */
static void test_value_fidelity(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE f(id INTEGER PRIMARY KEY, code TEXT);")) == 0);
    assert(rc(cwist_orm_exec(
               orm, "INSERT INTO f VALUES (1234567890123456, '007'), (2, ' 12'), (3, '-5');")) ==
           0);
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
                                  "(3, 1.5, NULL), (4, 0, '-0'), "
                                  "(5, 0, '+1'), (6, 0, ' 1'), (7, 0, '01'), (8, 0, '-01');")) ==
           0);
    int64_t v = 7;
    assert(rc(cwist_orm_select_one_i64(orm, "x", "v", "id = 1", &v)) == 0);
    assert(v == INT64_C(9007199254740993));
    long long legacy = 0;
    assert(rc(cwist_orm_select_one_llong(orm, "x", "v", "id = 1", &legacy)) == 0);
    assert(legacy == 9007199254740993LL);
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
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 5", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 6", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 7", &v)) == CWIST_ERROR_PROTOCOL);
    assert(rc(cwist_orm_select_one_i64(orm, "x", "t", "id = 8", &v)) == CWIST_ERROR_PROTOCOL);
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

    /* Oracle dialect: RETURNING clause (like Postgres) */
    cwist_orm_use_dialect(orm, CWIST_ORM_ORACLE);
    id = 0;
    assert(rc(cwist_orm_insert_returning_int(orm, "d", row, "id", &id)) == 0 && id == 4);

    /* MS-SQL dialect: SCOPE_IDENTITY() does not exist in SQLite, so like MySQL it fails cleanly */
    cwist_orm_use_dialect(orm, CWIST_ORM_MSSQL);
    id = 0;
    assert(rc(cwist_orm_insert_returning_int(orm, "d", row, "id", &id)) != 0);

    /* Provider names */
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_POSTGRES), "PostgreSQL") == 0);
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_MYSQL), "MySQL") == 0);
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_MARIADB), "MariaDB") == 0);
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_ORACLE), "OracleDB") == 0);
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_MSSQL), "MS-SQL") == 0);
    assert(strcmp(cwist_rdbms_provider_name(CWIST_RDBMS_NONE), "None") == 0);

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
    assert(rc(cwist_orm_exec(
               orm, "CREATE TABLE q(id INTEGER PRIMARY KEY, r REAL, s TEXT, b BLOB, n TEXT);")) ==
           0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO q VALUES "
                                  "(1, 2.5, 'hello', x'00ff10', NULL), "
                                  "(9007199254740993, -0.25, '007', x'', '');")) == 0);

    cwist_orm_result_t *res = NULL;
    assert(rc(cwist_orm_query_typed(orm, "SELECT id, r, s, b, n FROM q ORDER BY r DESC;", &res)) ==
           0);
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

    /* The legacy path keeps large integers as exact strings and '007' as text. */
    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(orm, "SELECT id, s FROM q WHERE id = 9007199254740993;", &rows)) ==
           0);
    cJSON *legacy_row = cJSON_GetArrayItem(rows, 0);
    cJSON *legacy_id = cJSON_GetObjectItem(legacy_row, "id");
    assert(cJSON_IsString(legacy_id) && strcmp(legacy_id->valuestring, "9007199254740993") == 0);
    assert(cJSON_IsString(cJSON_GetObjectItem(legacy_row, "s")));
    cJSON_Delete(rows);
    rows = NULL;

    assert(rc(cwist_orm_exec(orm, "CREATE TABLE embedded(v TEXT);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO embedded VALUES(CAST(x'610062' AS TEXT));")) == 0);
    assert(rc(cwist_orm_query(orm, "SELECT v FROM embedded;", &rows)) != 0 && rows == NULL);
    res = NULL;
    assert(rc(cwist_orm_query_typed(orm, "SELECT v FROM embedded;", &res)) != 0 && res == NULL);
    assert(rc(cwist_orm_query(orm, "SELECT b FROM raw;", &rows)) == 0 && rows != NULL);
    cJSON *legacy_blob = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "b");
    assert(cJSON_IsString(legacy_blob) && strcmp(legacy_blob->valuestring, "wP/uQg==") == 0);
    cJSON_Delete(rows);

    cwist_orm_close_socket(orm);
}

static void test_numeric_literal_fidelity(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE n(id INTEGER PRIMARY KEY, v REAL);")) == 0);
    cJSON *row = cJSON_CreateObject();
    cJSON_AddNumberToObject(row, "id", 1700000000123.0);
    cJSON_AddNumberToObject(row, "v", 0.12345678901234567);
    assert(rc(cwist_orm_insert(orm, "n", row)) == 0);
    cJSON_Delete(row);
    long long id = 0;
    double value = 0;
    assert(rc(cwist_orm_select_one_llong(orm, "n", "id", NULL, &id)) == 0);
    assert(id == 1700000000123LL);
    assert(rc(cwist_orm_select_one_double(orm, "n", "v", NULL, &value)) == 0);
    assert(value == 0.12345678901234567);
    row = cJSON_CreateObject();
    cJSON_AddNumberToObject(row, "id", NAN);
    assert(rc(cwist_orm_insert(orm, "n", row)) != 0);
    cJSON_Delete(row);
    cwist_orm_close_socket(orm);
}

static void test_mysql_literal_safety(void) {
    const char *payload = "\\' , is_admin) VALUES (1, 1) -- ";
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE m(id INTEGER PRIMARY KEY, value TEXT);")) == 0);
    const cwist_orm_dialect_t dialects[] = {CWIST_ORM_MYSQL, CWIST_ORM_MARIADB};
    for (size_t i = 0; i < 2; i++) {
        cwist_orm_use_dialect(orm, dialects[i]);
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "value", payload);
        assert(rc(cwist_orm_insert(orm, "m", row)) == 0);
        cJSON_Delete(row);
    }
    for (int id = 1; id <= 2; id++) {
        char where[32];
        snprintf(where, sizeof(where), "id = %d", id);
        char *value = NULL;
        assert(rc(cwist_orm_select_one_string(orm, "m", "value", where, &value)) == 0);
        assert(value != NULL && strcmp(value, payload) == 0);
        free(value);
    }
    cwist_orm_close_socket(orm);
}

static void test_json_literal_rejections(void) {
    cwist_orm_t *orm = open_memdb();
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE literals(v);")) == 0);
    cJSON *rounded = cJSON_CreateObject();
    cJSON_AddNumberToObject(rounded, "v", 9007199254740992.0);
    assert(rc(cwist_orm_insert(orm, "literals", rounded)) == CWIST_ERROR_NOMEM);
    cJSON_Delete(rounded);

    cJSON *safe = cJSON_CreateObject();
    cJSON_AddNumberToObject(safe, "v", 9007199254740991.0);
    assert(rc(cwist_orm_insert(orm, "literals", safe)) == 0);
    cJSON_Delete(safe);
    cwist_orm_close_socket(orm);
}

static void test_closed_peer_does_not_raise_sigpipe(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    close(fds[1]);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        cwist_orm_t *orm = cwist_orm_open_socket(fds[0]);
        if (!orm) _exit(2);
        cwist_error_t err = cwist_orm_exec(orm, "SELECT 1;");
        int failed = rc(err) != 0;
        cwist_orm_close_socket(orm);
        _exit(failed ? 0 : 1);
    }
    close(fds[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_commit_scope_and_interrupted_recv(void) {
    cwist_orm_immediate_commit(true);
    cwist_orm_t *writer = open_shared_memdb();
    cwist_orm_t *reader = NULL;
    cwist_orm_immediate_commit(false);
    assert(rc(cwist_orm_exec(writer, "CREATE TABLE commits(v INTEGER);")) == 0);
    assert(rc(cwist_orm_exec(writer, "BEGIN;")) == 0);
    assert(rc(cwist_orm_exec(writer, "INSERT INTO commits VALUES (1);")) == 0);
    assert(rc(cwist_orm_exec(writer, "INSERT INTO commits VALUES (2);")) == 0);
    reader = open_shared_memdb();
    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(reader, "SELECT COUNT(*) AS n FROM commits;", &rows)) == 0);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "n")->valueint == 2);
    cJSON_Delete(rows);
    rows = NULL;

    assert(rc(cwist_orm_exec(writer, "BEGIN;")) == 0);
    assert(rc(cwist_orm_exec(writer, "INSERT INTO commits VALUES (99); ROLLBACK;")) ==
           CWIST_ERROR_PROTOCOL);
    inject_recv_eintr = 1;
    assert(rc(cwist_orm_query(writer, "SELECT 1 AS n;", &rows)) == 0);
    cJSON_Delete(rows);
    cwist_orm_close_socket(writer);
    cwist_orm_close_socket(reader);
    next_shared_db();

    cwist_orm_t *uncommitted = open_shared_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(uncommitted, "CREATE TABLE deferred(v INTEGER);")) == 0);
    reader = open_shared_memdb();
    assert(rc(cwist_orm_exec(uncommitted, "BEGIN;")) == 0);
    assert(rc(cwist_orm_exec(uncommitted, "INSERT INTO deferred VALUES (1);")) == 0);
    assert(rc(cwist_orm_query(reader, "SELECT COUNT(*) AS n FROM deferred;", &rows)) == 0);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "n")->valueint == 0);
    cJSON_Delete(rows);
    cwist_orm_close_socket(uncommitted);
    cwist_orm_close_socket(reader);
    cwist_orm_immediate_commit(false);
}

static void test_numeric_ranges(void) {
    cwist_orm_t *orm = open_memdb();
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE ranges(v REAL);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO ranges VALUES (1e100), (-1e100), (1.5);")) == 0);
    int i = 7;
    long long ll = 7;
    assert(rc(cwist_orm_select_one_int(orm, "ranges", "v", "v = 1e100", &i)) ==
               CWIST_ERROR_PROTOCOL &&
           i == 7);
    assert(rc(cwist_orm_select_one_llong(orm, "ranges", "v", "v = -1e100", &ll)) ==
               CWIST_ERROR_PROTOCOL &&
           ll == 7);
    assert(rc(cwist_orm_select_one_int(orm, "ranges", "v", "v = 1.5", &i)) == CWIST_ERROR_PROTOCOL);
    cwist_orm_close_socket(orm);
}

static void test_typed_multi_statement(void) {
    cwist_orm_t *orm = open_memdb();
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE multi(a INTEGER, b INTEGER);")) == 0);
    assert(rc(cwist_orm_exec(orm, "INSERT INTO multi VALUES (1, 2), (3, 4);")) == 0);
    cwist_orm_result_t *res = NULL;
    assert(rc(cwist_orm_query_typed(orm, "SELECT a, b FROM multi; SELECT b FROM multi;", &res)) ==
           0);
    assert(res && res->column_count == 1 && res->row_count == 2);
    assert(strcmp(res->columns[0], "b") == 0);
    const cwist_orm_value_t *cell = cwist_orm_result_cell(res, 0, 0);
    assert(cell && cell->type == CWIST_ORM_TYPE_INTEGER && cell->i64 == 2);
    cwist_orm_result_free(res);
    cwist_orm_close_socket(orm);
}

static int peer_read_exact(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int peer_write_exact(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static void test_malformed_rows_shape(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        uint32_t net_len;
        if (peer_read_exact(sv[1], &net_len, sizeof(net_len)) != 0) _exit(2);
        uint32_t len = ntohl(net_len);
        char sql[64];
        if (len >= sizeof(sql) || peer_read_exact(sv[1], sql, len) != 0) _exit(3);
        static const char payload[] = "{\"status\":0,\"rows\":\"bad\"}";
        uint32_t response_len = htonl(sizeof(payload) - 1);
        int failed = peer_write_exact(sv[1], &response_len, sizeof(response_len)) != 0 ||
                     peer_write_exact(sv[1], payload, sizeof(payload) - 1) != 0;
        close(sv[1]);
        _exit(failed ? 4 : 0);
    }
    close(sv[1]);
    cwist_orm_t *orm = cwist_orm_open_socket(sv[0]);
    assert(orm);
    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(orm, "SELECT 1;", &rows)) == CWIST_ERROR_PROTOCOL && rows == NULL);
    cwist_orm_close_socket(orm);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void mock_query_payload(const char *payload, bool typed, bool success) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        uint32_t net_len;
        if (peer_read_exact(sv[1], &net_len, sizeof(net_len)) != 0) _exit(2);
        uint32_t len = ntohl(net_len);
        char sql[64];
        if (len >= sizeof(sql) || peer_read_exact(sv[1], sql, len) != 0) _exit(3);
        size_t payload_len = strlen(payload);
        uint32_t response_len = htonl((uint32_t)payload_len);
        int failed = peer_write_exact(sv[1], &response_len, sizeof(response_len)) != 0 ||
                     peer_write_exact(sv[1], payload, payload_len) != 0;
        close(sv[1]);
        _exit(failed ? 4 : 0);
    }
    close(sv[1]);
    cwist_orm_t *orm = cwist_orm_open_socket(sv[0]);
    assert(orm);
    if (typed) {
        cwist_orm_result_t *res = NULL;
        cwist_error_t err = cwist_orm_query_typed(orm, "SELECT 1;", &res);
        if (success) {
            assert(rc(err) == 0 && res && res->row_count == 1 && res->column_count == 1);
            const cwist_orm_value_t *cell = cwist_orm_result_cell(res, 0, 0);
            assert(cell && cell->type == CWIST_ORM_TYPE_BLOB && cell->blob_len == 1 &&
                   cell->blob[0] == 0);
        } else {
            assert(rc(err) == CWIST_ERROR_PROTOCOL && res == NULL);
        }
        cwist_orm_result_free(res);
    } else {
        cJSON *rows = NULL;
        assert(rc(cwist_orm_query(orm, "SELECT 1;", &rows)) == CWIST_ERROR_PROTOCOL &&
               rows == NULL);
    }
    cwist_orm_close_socket(orm);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_status_and_blob_validation(void) {
    mock_query_payload("{\"status\":0.5,\"rows\":[]}", false, false);
    static const char invalid1[] =
        "{\"status\":0,\"typed\":{\"columns\":[\"b\"],\"rows\":[[{\"t\":\"blob\",\"v\":\"AB==\",\"n\":1}]]},\"rows\":[]}";
    static const char invalid2[] =
        "{\"status\":0,\"typed\":{\"columns\":[\"b\"],\"rows\":[[{\"t\":\"blob\",\"v\":\"AAB=\",\"n\":1}]]},\"rows\":[]}";
    static const char valid[] =
        "{\"status\":0,\"typed\":{\"columns\":[\"b\"],\"rows\":[[{\"t\":\"blob\",\"v\":\"AA==\",\"n\":1}]]},\"rows\":[]}";
    mock_query_payload(invalid1, true, false);
    mock_query_payload(invalid2, true, false);
    mock_query_payload(valid, true, true);
}

static void test_db_open_allocation_failure(void) {
    fail_cwist_alloc = 1;
    cwist_db *db = (cwist_db *)1;
    assert(cwist_db_open(&db, ":memory:").error.err_i16 != 0 && db == NULL);
    db = (cwist_db *)1;
    assert(cwist_db_open_memory(&db, "x", 1, 0).error.err_i16 != 0 && db == NULL);
    fail_cwist_alloc = 0;
}

static void test_json_print_failure_is_not_sql_null(void) {
    cwist_orm_t *orm = open_memdb();
    cwist_orm_immediate_commit(true);
    assert(rc(cwist_orm_exec(orm, "CREATE TABLE j(v TEXT);")) == 0);
    cJSON *row = cJSON_CreateObject();
    assert(cJSON_AddObjectToObject(row, "v") != NULL);
    fail_json_print = 1;
    cwist_error_t err = cwist_orm_insert(orm, "j", row);
    fail_json_print = 0;
    assert(rc(err) != 0);
    cJSON_Delete(row);

    cJSON *rows = NULL;
    assert(rc(cwist_orm_query(orm, "SELECT COUNT(*) AS c FROM j;", &rows)) == 0);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "c")->valuedouble == 0);
    cJSON_Delete(rows);
    cwist_orm_close_socket(orm);
    cwist_orm_immediate_commit(false);
}

int main(void) {
    test_json_print_failure_is_not_sql_null();
    test_query_and_transactions();
    test_insert_returning();
    test_select_one();
    test_value_fidelity();
    test_exact_i64();
    test_query_typed();
    test_dialects();
    test_numeric_literal_fidelity();
    test_mysql_literal_safety();
    test_json_literal_rejections();
    test_closed_peer_does_not_raise_sigpipe();
    test_commit_scope_and_interrupted_recv();
    test_numeric_ranges();
    test_typed_multi_statement();
    test_malformed_rows_shape();
    test_status_and_blob_validation();
    test_db_open_allocation_failure();
    cwist_orm_immediate_commit(false);
    printf("test_orm: ok\n");
    return 0;
}
