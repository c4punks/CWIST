#include <cwist/sql.h>
#include <cwist/err/cwist_err.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cwist_error_t make_sqlite_error(int rc, char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(err.error.err_json, "sqlite_rc", rc);
    cJSON_AddStringToObject(err.error.err_json, "message", msg ? msg : "Unknown Error");
    return err;
}

cwist_error_t cwist_db_open(cwist_db **db, const char *path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!db || !path) {
        err.error.err_i16 = -1;
        return err;
    }

    *db = (cwist_db*)malloc(sizeof(cwist_db));
    if (!*db) {
        err.error.err_i16 = -1;
        return err;
    }

    int rc = sqlite3_open(path, &(*db)->conn);
    if (rc) {
        cwist_error_t sql_err = make_sqlite_error(rc, (char*)sqlite3_errmsg((*db)->conn));
        sqlite3_close((*db)->conn);
        free(*db);
        *db = NULL;
        return sql_err;
    }

    err.error.err_i16 = 0;
    return err;
}

void cwist_db_close(cwist_db *db) {
    if (db) {
        if (db->conn) {
            sqlite3_close(db->conn);
        }
        free(db);
    }
}

cwist_error_t cwist_db_exec(cwist_db *db, const char *sql) {
    char *zErrMsg = 0;
    int rc = sqlite3_exec(db->conn, sql, 0, 0, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        cwist_error_t err = make_sqlite_error(rc, zErrMsg);
        sqlite3_free(zErrMsg);
        return err;
    }

    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = 0;
    return err;
}

// Callback for converting rows to JSON
typedef struct {
    cJSON *rows;
} query_context;

static int query_callback(void *data, int argc, char **argv, char **azColName) {
    query_context *ctx = (query_context *)data;
    cJSON *row = cJSON_CreateObject();
    
    for (int i = 0; i < argc; i++) {
        // SQLite returns everything as char* by default in exec callback
        // For simplicity, we treat everything as string or try to guess types if needed.
        // But for a generic wrapper, string is safest unless we parse schema.
        if (argv[i]) {
            cJSON_AddStringToObject(row, azColName[i], argv[i]);
        } else {
            cJSON_AddNullToObject(row, azColName[i]);
        }
    }
    
    cJSON_AddItemToArray(ctx->rows, row);
    return 0;
}

cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result) {
    if (!db || !sql || !result) {
        cwist_error_t err = make_error(CWIST_ERR_INT16);
        err.error.err_i16 = -1;
        return err;
    }

    query_context ctx;
    ctx.rows = cJSON_CreateArray();
    
    char *zErrMsg = 0;
    int rc = sqlite3_exec(db->conn, sql, query_callback, &ctx, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        cJSON_Delete(ctx.rows);
        cwist_error_t err = make_sqlite_error(rc, zErrMsg);
        sqlite3_free(zErrMsg);
        return err;
    }

    *result = ctx.rows;
    
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = 0;
    return err;
}
