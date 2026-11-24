/**
 * @file pg.c
 * @brief PostgreSQL client integration using libpq.
 */

#include <cwist/core/db/pg.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

#if defined(CWIST_HAS_LIBPQ)
#if defined(__has_include)
#if __has_include(<libpq-fe.h>)
#define CWIST_INTERNAL_LIBPQ 1
#include <libpq-fe.h>
#elif __has_include(<postgresql/libpq-fe.h>)
#define CWIST_INTERNAL_LIBPQ 1
#include <postgresql/libpq-fe.h>
#endif
#else
#define CWIST_INTERNAL_LIBPQ 1
#include <libpq-fe.h>
#endif
#endif

#if defined(CWIST_INTERNAL_LIBPQ)

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

static long long pg_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int pg_wait_until(cwist_pg_t *pg, short events, long long deadline) {
    int fd = PQsocket(pg->conn);
    if (fd < 0) return -1;
    for (;;) {
        long long remaining = deadline - pg_now_ms();
        if (remaining <= 0) {
            set_last_error(pg, "database operation timed out");
            return 0;
        }
        struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
        int rc = poll(&pfd, 1, remaining > 2147483647 ? 2147483647 : (int)remaining);
        if (rc > 0) return 1;
        if (rc == 0) {
            set_last_error(pg, "database operation timed out");
            return 0;
        }
        if (errno != EINTR) {
            set_last_error(pg, strerror(errno));
            return -1;
        }
    }
}

static void pg_abort_connection(cwist_pg_t *pg) {
    if (pg->conn) {
        PQfinish(pg->conn);
        pg->conn = NULL;
    }
}

static int pg_flush(cwist_pg_t *pg, long long deadline) {
    int rc;
    while ((rc = PQflush(pg->conn)) == 1) {
        if (pg_wait_until(pg, POLLOUT, deadline) != 1) return -1;
    }
    if (rc < 0) set_last_error(pg, PQerrorMessage(pg->conn));
    return rc;
}

static PGresult *pg_results(cwist_pg_t *pg, const char *sql, int n_params,
                            const char *const *param_values) {
    long long deadline = pg_now_ms() + 30000;
    int sent = n_params >= 0 ? PQsendQueryParams(pg->conn, sql, n_params, NULL,
                                                 param_values, NULL, NULL, 0)
                             : PQsendQuery(pg->conn, sql);
    if (sent != 1) {
        set_last_error(pg, PQerrorMessage(pg->conn));
        return NULL;
    }
    if (pg_flush(pg, deadline) < 0) return NULL;

    PGresult *last = NULL;
    for (;;) {
        while (PQisBusy(pg->conn)) {
            if (!PQconsumeInput(pg->conn)) {
                set_last_error(pg, PQerrorMessage(pg->conn));
                goto fail;
            }
            if (pg_wait_until(pg, POLLIN, deadline) != 1) goto fail;
        }
        PGresult *next = PQgetResult(pg->conn);
        if (!next) return last;
        PQclear(last);
        last = next;
    }

fail:
    PQclear(last);
    return NULL;
}

cwist_pg_t *cwist_pg_connect(const char *conninfo) {
    if (!conninfo || conninfo[0] == '\0') return NULL;

    cwist_pg_t *pg = (cwist_pg_t *)cwist_alloc(sizeof(*pg));
    if (!pg) return NULL;
    memset(pg, 0, sizeof(*pg));

    pg->conn = PQconnectStart(conninfo);
    if (!pg->conn) {
        cwist_free(pg);
        return NULL;
    }
    long long deadline = pg_now_ms() + 5000;
    for (;;) {
        PostgresPollingStatusType status = PQconnectPoll(pg->conn);
        if (status == PGRES_POLLING_OK) break;
        if (status == PGRES_POLLING_FAILED) {
            set_last_error(pg, PQerrorMessage(pg->conn));
            pg_abort_connection(pg);
            cwist_free(pg);
            return NULL;
        }
        short events = status == PGRES_POLLING_READING ? POLLIN : POLLOUT;
        if (pg_wait_until(pg, events, deadline) != 1) {
            pg_abort_connection(pg);
            cwist_free(pg);
            return NULL;
        }
    }
    if (PQsetnonblocking(pg->conn, 1) != 0) {
        set_last_error(pg, PQerrorMessage(pg->conn));
        pg_abort_connection(pg);
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

    PGresult *res = pg_results(pg, sql, -1, NULL);
    if (!res) {
        pg_abort_connection(pg);
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
    cJSON *rows = NULL;
    if (out_rows) *out_rows = NULL;
    if (!pg || !pg->conn || !sql || n_params < 0 || (n_params > 0 && !param_values)) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    PGresult *res = pg_results(pg, sql, n_params, param_values);
    if (!res) {
        pg_abort_connection(pg);
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
        rows = cJSON_CreateArray();
        if (!rows) goto oom;
        int n_rows = PQntuples(res);
        int n_fields = PQnfields(res);

        for (int i = 0; i < n_rows; i++) {
            cJSON *row = cJSON_CreateObject();
            if (!row) goto oom;
            for (int j = 0; j < n_fields; j++) {
                const char *field_name = PQfname(res, j);
                cJSON *cell = PQgetisnull(res, i, j)
                                  ? cJSON_AddNullToObject(row, field_name)
                                  : cJSON_AddStringToObject(row, field_name, PQgetvalue(res, i, j));
                if (!cell) {
                    cJSON_Delete(row);
                    goto oom;
                }
            }
            if (!cJSON_AddItemToArray(rows, row)) {
                cJSON_Delete(row);
                goto oom;
            }
        }
        *out_rows = rows;
    }

    PQclear(res);
    set_last_error(pg, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    return err;

oom:
    cJSON_Delete(rows);
    PQclear(res);
    set_last_error(pg, "out of memory");
    err.error.err_i16 = CWIST_ERROR_NOMEM;
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

/* Stubs when CWIST_HAS_LIBPQ is not enabled or libpq header is absent */
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
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_pg_exec_params(cwist_pg_t *pg, const char *sql, int n_params,
                                   const char *const *param_values, cJSON **out_rows) {
    (void)pg; (void)sql; (void)n_params; (void)param_values;
    if (out_rows) *out_rows = NULL;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_pg_begin(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_pg_commit(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_pg_rollback(cwist_pg_t *pg) {
    (void)pg;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

const char *cwist_pg_last_error(cwist_pg_t *pg) {
    (void)pg;
    return "libpq support is disabled";
}

#endif
