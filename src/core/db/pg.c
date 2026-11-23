/**
 * @file pg.c
 * @brief PostgreSQL client integration using libpq.
 */

#include <cwist/core/db/pg.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CWIST_HAS_LIBPQ)
#include <libpq-fe.h>

struct cwist_pg {
    PGconn *conn;
    char last_error[512];
};

static void set_last_error(cwist_pg_t *pg, const char *msg) {
    if (!pg) return;
    if (msg) {
        snprintf(pg->last_error, sizeof(pg->last_error), "%s", msg);
    } else {
        pg->last_error[0] = '\0';
    }
}

cwist_pg_t *cwist_pg_connect(const char *conninfo) {
    if (!conninfo || conninfo[0] == '\0') return NULL;

    cwist_pg_t *pg = (cwist_pg_t *)cwist_alloc(sizeof(*pg));
    if (!pg) return NULL;
    memset(pg, 0, sizeof(*pg));

    pg->conn = PQconnectdb(conninfo);
    if (!pg->conn) {
        cwist_free(pg);
        return NULL;
    }

    if (PQstatus(pg->conn) != CONNECTION_OK) {
        set_last_error(pg, PQerrorMessage(pg->conn));
        PQfinish(pg->conn);
        cwist_free(pg);
        return NULL;
    }

    set_last_error(pg, NULL);
    return pg;
}

void cwist_pg_disconnect(cwist_pg_t *pg) {
    if (!pg) return;
    if (pg->conn) {
        PQfinish(pg->conn);
        pg->conn = NULL;
    }
    cwist_free(pg);
}

bool cwist_pg_is_connected(cwist_pg_t *pg) {
    if (!pg || !pg->conn) return false;
    return PQstatus(pg->conn) == CONNECTION_OK;
}

cwist_error_t cwist_pg_exec(cwist_pg_t *pg, const char *sql) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!pg || !pg->conn || !sql) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    PGresult *res = PQexec(pg->conn, sql);
    if (!res) {
        set_last_error(pg, PQerrorMessage(pg->conn));
        err.error.err_i16 = CWIST_ERROR_IO;
        return err;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_last_error(pg, PQresultErrorMessage(res));
        PQclear(res);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    PQclear(res);
    set_last_error(pg, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

cwist_error_t cwist_pg_exec_params(cwist_pg_t *pg, const char *sql, int n_params,
                                   const char *const *param_values, cJSON **out_rows) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (out_rows) *out_rows = NULL;
    if (!pg || !pg->conn || !sql) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    PGresult *res = PQexecParams(pg->conn, sql, n_params, NULL,
                                 param_values, NULL, NULL, 0);
    if (!res) {
        set_last_error(pg, PQerrorMessage(pg->conn));
        err.error.err_i16 = CWIST_ERROR_IO;
        return err;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_last_error(pg, PQresultErrorMessage(res));
        PQclear(res);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    if (out_rows) {
        cJSON *rows = cJSON_CreateArray();
        int n_rows = PQntuples(res);
        int n_fields = PQnfields(res);

        for (int i = 0; i < n_rows; i++) {
            cJSON *row = cJSON_CreateObject();
            for (int j = 0; j < n_fields; j++) {
                const char *field_name = PQfname(res, j);
                if (PQgetisnull(res, i, j)) {
                    cJSON_AddNullToObject(row, field_name);
                } else {
                    const char *val = PQgetvalue(res, i, j);
                    cJSON_AddStringToObject(row, field_name, val);
                }
            }
            cJSON_AddItemToArray(rows, row);
        }
        *out_rows = rows;
    }

    PQclear(res);
    set_last_error(pg, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

cwist_error_t cwist_pg_begin(cwist_pg_t *pg) {
    return cwist_pg_exec(pg, "BEGIN;");
}

cwist_error_t cwist_pg_commit(cwist_pg_t *pg) {
    return cwist_pg_exec(pg, "COMMIT;");
}

cwist_error_t cwist_pg_rollback(cwist_pg_t *pg) {
    return cwist_pg_exec(pg, "ROLLBACK;");
}

const char *cwist_pg_last_error(cwist_pg_t *pg) {
    if (!pg) return "";
    return pg->last_error;
}

#else

/* Stubs when CWIST_HAS_LIBPQ is not enabled */
cwist_pg_t *cwist_pg_connect(const char *conninfo) {
    (void)conninfo;
    return NULL;
}

void cwist_pg_disconnect(cwist_pg_t *pg) {
    (void)pg;
}

bool cwist_pg_is_connected(cwist_pg_t *pg) {
    (void)pg;
    return false;
}

cwist_error_t cwist_pg_exec(cwist_pg_t *pg, const char *sql) {
    (void)pg; (void)sql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = CWIST_ERROR_NOT_SUPPORTED;
    return err;
}

cwist_error_t cwist_pg_exec_params(cwist_pg_t *pg, const char *sql, int n_params,
                                   const char *const *param_values, cJSON **out_rows) {
    (void)pg; (void)sql; (void)n_params; (void)param_values;
    if (out_rows) *out_rows = NULL;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = CWIST_ERROR_NOT_SUPPORTED;
    return err;
}

cwist_error_t cwist_pg_begin(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = CWIST_ERROR_NOT_SUPPORTED;
    return err;
}

cwist_error_t cwist_pg_commit(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = CWIST_ERROR_NOT_SUPPORTED;
    return err;
}

cwist_error_t cwist_pg_rollback(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = CWIST_ERROR_NOT_SUPPORTED;
    return err;
}

const char *cwist_pg_last_error(cwist_pg_t *pg) {
    (void)pg;
    return "libpq support is disabled";
}

#endif
