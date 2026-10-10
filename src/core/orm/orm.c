/**
 * @file orm.c
 * @brief Tiny ORM client over a framed Unix socket stream.
 *
 * This implementation never includes @c sqlite3.h.  All communication
 * with the SQL backend is serialised through a socket produced by
 * cwist_db_transfer_sqlite_to_socket().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/core/orm/orm.h>
#include <cwist/core/mem/alloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <stdint.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------------- */
/* Internal ORM handle                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief ORM session state.
 */
struct cwist_orm {
    int sock_fd;          /**< Connected Unix socket descriptor. */
    bool auto_commit;      /**< Mirrored immediate-commit flag. */
    cwist_orm_dialect_t dialect; /**< SQL dialect for query generation. */
};

/** @brief Global immediate-commit default. */
static volatile bool g_immediate_commit = false;

/* ------------------------------------------------------------------------- */
/* Framing helpers                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Receive exactly @p n bytes from @p fd.
 * @return 0 on success, -1 on EOF or transport error.
 */
static int orm_recv_all(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, MSG_WAITALL);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

/**
 * @brief Send exactly @p n bytes to @p fd.
 * @return 0 on success, -1 on transport error.
 */
static int orm_send_all(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < n) {
        ssize_t w = send(fd, p + total, n - total, 0);
        if (w <= 0) return -1;
        total += (size_t)w;
    }
    return 0;
}

/**
 * @brief Send a framed SQL request and receive the framed JSON response.
 *
 * On success the caller receives a heap-allocated null-terminated JSON
 * string that must be freed with @c free().  On failure @c *out_payload
 * is set to @c NULL.
 *
 * @param orm         Active ORM session.
 * @param sql         Null-terminated SQL statement.
 * @param out_payload [out] Receives the JSON response string.
 * @return 0 on success, -1 on I/O or protocol failure.
 */
static int orm_exchange(cwist_orm_t *orm, const char *sql, char **out_payload) {
    *out_payload = NULL;
    if (!orm || orm->sock_fd < 0 || !sql) return -1;

    size_t sql_len = strlen(sql);
    if (sql_len > UINT32_MAX - 1) return -1;

    uint32_t net_len = htonl((uint32_t)sql_len);
    if (orm_send_all(orm->sock_fd, &net_len, sizeof(net_len)) != 0) return -1;
    if (orm_send_all(orm->sock_fd, sql, sql_len) != 0) return -1;

    uint32_t net_payload = 0;
    if (orm_recv_all(orm->sock_fd, &net_payload, sizeof(net_payload)) != 0) return -1;
    uint32_t payload_len = ntohl(net_payload);
    if (payload_len > 64 * 1024 * 1024) return -1; /* sanity ceiling */

    char *payload = (char *)malloc(payload_len + 1);
    if (!payload) return -1;
    if (payload_len > 0 && orm_recv_all(orm->sock_fd, payload, payload_len) != 0) {
        free(payload);
        return -1;
    }
    payload[payload_len] = '\0';
    *out_payload = payload;
    return 0;
}

/**
 * @brief Parse the status field out of a worker JSON response.
 *
 * @param payload Response string (must be valid JSON).
 * @return The integer status code, or -1 if parsing fails.
 */
static int orm_parse_status(const char *payload) {
    if (!payload) return -1;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return -1;
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    int rc = (cJSON_IsNumber(status)) ? (int)status->valuedouble : -1;
    cJSON_Delete(root);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Wrap a connected socket descriptor in an ORM session.
 *
 * Takes ownership of @p socket_fd: the descriptor is closed by
 * cwist_orm_close_socket().  The session inherits the current global
 * immediate-commit default and defaults to the SQLite dialect.
 *
 * @param socket_fd Connected socket descriptor (must be >= 0).
 * @return New ORM handle, or NULL on invalid descriptor or allocation failure.
 */
cwist_orm_t *cwist_orm_open_socket(int socket_fd) {
    if (socket_fd < 0) return NULL;
    cwist_orm_t *orm = (cwist_orm_t *)calloc(1, sizeof(*orm));
    if (!orm) return NULL;
    orm->sock_fd = socket_fd;
    orm->auto_commit = g_immediate_commit;
    orm->dialect = CWIST_ORM_SQLITE;
    return orm;
}

/**
 * @brief Close an ORM session and release its socket.
 *
 * Issues a best-effort ROLLBACK to discard any dangling transaction, then
 * closes the socket descriptor and frees the handle.  Safe to call with NULL.
 *
 * @param orm Session to close (may be NULL).
 */
void cwist_orm_close_socket(cwist_orm_t *orm) {
    if (!orm) return;
    if (orm->sock_fd >= 0) {
        /* Best-effort rollback of any dangling transaction. */
        char *dummy = NULL;
        (void)orm_exchange(orm, "ROLLBACK;", &dummy);
        free(dummy);
        close(orm->sock_fd);
    }
    free(orm);
}

/* ------------------------------------------------------------------------- */
/* Transaction control                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Set the global immediate-commit default for new ORM sessions.
 *
 * Sessions opened afterwards inherit this value; existing sessions are not
 * affected.  When enabled, INSERT/UPDATE/DELETE statements executed through
 * cwist_orm_exec() are followed by an automatic COMMIT.
 *
 * @param enable true to enable immediate commit, false to disable.
 */
void cwist_orm_immediate_commit(bool enable) {
    g_immediate_commit = enable;
}

/**
 * @brief Select the SQL dialect used for query generation.
 *
 * Affects identifier quoting and dialect-specific statements generated by
 * the high-level helpers.  Does nothing if @p orm is NULL.
 *
 * @param orm     Session to configure.
 * @param dialect Dialect to use (e.g. CWIST_ORM_SQLITE).
 */
void cwist_orm_use_dialect(cwist_orm_t *orm, cwist_orm_dialect_t dialect) {
    if (orm) orm->dialect = dialect;
}

/**
 * @brief Commit the current transaction.
 *
 * @param orm Active session.
 * @return CWIST_SUCCESS on success, otherwise an error code.
 */
cwist_error_t cwist_orm_commit(cwist_orm_t *orm) {
    return cwist_orm_exec(orm, "COMMIT;");
}

/**
 * @brief Roll back the current transaction.
 *
 * @param orm Active session.
 * @return CWIST_SUCCESS on success, otherwise an error code.
 */
cwist_error_t cwist_orm_rollback(cwist_orm_t *orm) {
    return cwist_orm_exec(orm, "ROLLBACK;");
}

/* ------------------------------------------------------------------------- */
/* Low-level SQL                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Execute a single SQL statement, discarding any result rows.
 *
 * If immediate-commit mode is enabled, an INSERT/UPDATE/DELETE statement is
 * followed by an automatic COMMIT.
 *
 * @param orm Active session.
 * @param sql Null-terminated SQL statement.
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_IO on transport failure, or
 *         CWIST_ERROR_PROTOCOL when the backend reports an error.
 */
cwist_error_t cwist_orm_exec(cwist_orm_t *orm, const char *sql) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!orm || !sql) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *payload = NULL;
    if (orm_exchange(orm, sql, &payload) != 0) {
        err.error.err_i16 = CWIST_ERROR_IO;
        return err;
    }

    int status = orm_parse_status(payload);
    free(payload);

    if (status != 0) {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    /* Auto-commit write helpers if requested. */
    if (g_immediate_commit) {
        const char *upper = sql;
        while (*upper && *upper <= ' ') upper++;
        if ((strncasecmp(upper, "INSERT", 6) == 0) || (strncasecmp(upper, "UPDATE", 6) == 0) ||
            (strncasecmp(upper, "DELETE", 6) == 0)) {
            char *commit_payload = NULL;
            (void)orm_exchange(orm, "COMMIT;", &commit_payload);
            free(commit_payload);
        }
    }

    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

/**
 * @brief Execute a SELECT-style query and return the result rows.
 *
 * On success @p *result receives a cJSON array of row objects (empty array
 * if no rows) that the caller must release with cJSON_Delete().
 *
 * @param orm    Active session.
 * @param sql    Null-terminated SQL query.
 * @param result [out] Receives the rows array.
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_IO on transport failure, or
 *         CWIST_ERROR_PROTOCOL on backend error or malformed response.
 */
cwist_error_t cwist_orm_query(cwist_orm_t *orm, const char *sql, cJSON **result) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (result) *result = NULL;
    if (!orm || !sql || !result) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *payload = NULL;
    if (orm_exchange(orm, sql, &payload) != 0) {
        err.error.err_i16 = CWIST_ERROR_IO;
        return err;
    }

    cJSON *root = cJSON_Parse(payload);
    free(payload);
    if (!root) {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsNumber(status) || (int)status->valuedouble != 0) {
        cJSON_Delete(root);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    cJSON *rows = cJSON_DetachItemFromObjectCaseSensitive(root, "rows");
    cJSON_Delete(root);
    if (!rows) {
        /* Guarantee an array even on empty result. */
        rows = cJSON_CreateArray();
    }

    *result = rows;
    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

/* ------------------------------------------------------------------------- */
/* SQL value formatter                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Escape a C string for safe use inside a single-quoted SQLite literal.
 *
 * SQLite escapes a single quote by doubling it ('').
 *
 * @param src Raw input string (must not be NULL).
 * @return Heap-allocated escaped string.  Caller must free().
 */
static char *cwist_orm_escape_sqlite(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\'') extra++;
    }
    char *dst = (char *)malloc(len + extra + 1);
    if (!dst) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\'') dst[j++] = '\'';
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return dst;
}

/**
 * @brief Quote an SQL identifier according to the active dialect.
 *
 * PostgreSQL uses double quotes, MySQL/MariaDB/SQLite use backticks.
 *
 * @param orm ORM session (determines dialect).
 * @param id  Raw identifier string.
 * @return Heap-allocated quoted identifier, or NULL on failure.
 */
static char *cwist_orm_quote_identifier(const cwist_orm_t *orm, const char *id) {
    if (!orm || !id) return NULL;
    char quote_ch = '\0';
    char escape_ch = '\0';
    switch (orm->dialect) {
        case CWIST_ORM_POSTGRES:
            quote_ch = '"';
            escape_ch = '"';
            break;
        case CWIST_ORM_MYSQL:
        case CWIST_ORM_MARIADB:
        case CWIST_ORM_SQLITE:
            quote_ch = '`';
            escape_ch = '`';
            break;
        default: return strdup(id);
    }

    size_t len = strlen(id);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (id[i] == quote_ch) extra++;
    }
    char *out = (char *)malloc(len + extra + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = quote_ch;
    for (size_t i = 0; i < len; i++) {
        if (id[i] == quote_ch) out[j++] = escape_ch;
        out[j++] = id[i];
    }
    out[j++] = quote_ch;
    out[j] = '\0';
    return out;
}

/**
 * @brief Convert a single cJSON node into an SQL literal fragment.
 *
 * Maps cJSON types to SQL literals (NULL, booleans as TRUE/FALSE or 1/0 by
 * dialect, numbers via %g, strings single-quoted with quote doubling, and
 * arrays/objects as single-quoted JSON text).  The returned string is
 * heap-allocated and must be freed by the caller.
 *
 * @param orm  Session (determines boolean rendering; may be NULL).
 * @param node cJSON value node (may be NULL, treated as SQL NULL).
 * @return SQL literal string, or NULL on allocation failure.
 */
static char *cwist_orm_json_to_sql_literal(const cwist_orm_t *orm, const cJSON *node) {
    if (!node) return strdup("NULL");

    if (cJSON_IsNull(node)) {
        return strdup("NULL");
    }
    if (cJSON_IsBool(node)) {
        if (orm && orm->dialect == CWIST_ORM_POSTGRES) {
            return strdup(cJSON_IsTrue(node) ? "TRUE" : "FALSE");
        }
        return strdup(cJSON_IsTrue(node) ? "1" : "0");
    }
    if (cJSON_IsNumber(node)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", node->valuedouble);
        return strdup(buf);
    }
    if (cJSON_IsString(node)) {
        if (!node->valuestring) return strdup("''");
        char *escaped = cwist_orm_escape_sqlite(node->valuestring);
        if (!escaped) return NULL;
        size_t elen = strlen(escaped);
        char *quoted = (char *)malloc(elen + 3);
        if (!quoted) {
            free(escaped);
            return NULL;
        }
        quoted[0] = '\'';
        memcpy(quoted + 1, escaped, elen);
        quoted[elen + 1] = '\'';
        quoted[elen + 2] = '\0';
        free(escaped);
        return quoted;
    }

    /* Fallback: stringify arrays/objects as JSON text. */
    char *raw = cJSON_PrintUnformatted((cJSON *)node);
    if (!raw) return strdup("NULL");
    char *escaped = cwist_orm_escape_sqlite(raw);
    free(raw);
    if (!escaped) return NULL;
    size_t elen = strlen(escaped);
    char *quoted = (char *)malloc(elen + 3);
    if (!quoted) {
        free(escaped);
        return NULL;
    }
    quoted[0] = '\'';
    memcpy(quoted + 1, escaped, elen);
    quoted[elen + 1] = '\'';
    quoted[elen + 2] = '\0';
    free(escaped);
    return quoted;
}

/* ------------------------------------------------------------------------- */
/* High-level helpers                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Build the core INSERT INTO ... VALUES (...) SQL fragment.
 *
 * The returned string does NOT end with a semicolon, so that callers
 * can append clauses such as RETURNING.
 *
 * @return Heap-allocated SQL string, or NULL on error.
 */
static char *orm_build_insert_sql(const cwist_orm_t *orm, const char *table, const cJSON *data) {
    if (!table || !data || !cJSON_IsObject(data)) return NULL;

    int col_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)data) col_count++;
    if (col_count == 0) return NULL;

    char *columns = (char *)malloc(1);
    char *values = (char *)malloc(1);
    if (!columns || !values) {
        free(columns);
        free(values);
        return NULL;
    }
    columns[0] = '\0';
    values[0] = '\0';

    char *qtable = cwist_orm_quote_identifier(orm, table);
    if (!qtable) {
        free(columns);
        free(values);
        return NULL;
    }

    int idx = 0;
    cJSON_ArrayForEach(item, (cJSON *)data) {
        char *literal = cwist_orm_json_to_sql_literal(orm, item);
        char *qid = cwist_orm_quote_identifier(orm, item->string);
        if (!literal || !qid) {
            free(literal);
            free(qid);
            free(qtable);
            free(columns);
            free(values);
            return NULL;
        }

        size_t need_col = strlen(columns) + strlen(qid) + 2;
        char *new_col = (char *)realloc(columns, need_col);
        if (!new_col) {
            free(literal);
            free(qid);
            free(qtable);
            free(columns);
            free(values);
            return NULL;
        }
        columns = new_col;

        size_t need_val = strlen(values) + strlen(literal) + 2;
        char *new_val = (char *)realloc(values, need_val);
        if (!new_val) {
            free(literal);
            free(qid);
            free(qtable);
            free(columns);
            free(values);
            return NULL;
        }
        values = new_val;

        if (idx > 0) {
            strcat(columns, ",");
            strcat(values, ",");
        }
        strcat(columns, qid);
        strcat(values, literal);
        free(literal);
        free(qid);
        idx++;
    }

    size_t sql_len = strlen(qtable) + strlen(columns) + strlen(values) + 32;
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(qtable);
        free(columns);
        free(values);
        return NULL;
    }
    snprintf(sql, sql_len, "INSERT INTO %s (%s) VALUES (%s)", qtable, columns, values);
    free(qtable);
    free(columns);
    free(values);
    return sql;
}

/**
 * @brief Insert a JSON object into a table.
 *
 * Column names and values are taken from the object's keys/values and
 * rendered according to the session dialect.
 *
 * @param orm   Active session.
 * @param table Table name.
 * @param data  JSON object with column names as keys.
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments or a non-object @p data, CWIST_ERROR_NOMEM on
 *         allocation failure, otherwise the error from cwist_orm_exec().
 */
cwist_error_t cwist_orm_insert(cwist_orm_t *orm, const char *table, const cJSON *data) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!orm || !table || !data || !cJSON_IsObject(data)) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *sql = orm_build_insert_sql(orm, table, data);
    if (!sql) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    size_t len = strlen(sql);
    char *final = (char *)realloc(sql, len + 2);
    if (!final) {
        free(sql);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    final[len] = ';';
    final[len + 1] = '\0';

    err = cwist_orm_exec(orm, final);
    free(final);
    return err;
}

/**
 * @brief Update rows of a table with values from a JSON object.
 *
 * Builds UPDATE table SET key=value,... from the object's entries; an empty
 * object is rejected.  If @p where_clause is non-empty it is appended as a
 * WHERE clause (a NULL or empty clause updates all rows).
 *
 * @param orm         Active session.
 * @param table       Table name.
 * @param data        JSON object with column names as keys (must be non-empty).
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments or an empty object, CWIST_ERROR_NOMEM on allocation
 *         failure, otherwise the error from cwist_orm_exec().
 */
cwist_error_t cwist_orm_update(cwist_orm_t *orm, const char *table, const cJSON *data,
                               const char *where_clause) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!orm || !table || !data || !cJSON_IsObject(data)) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *set_clause = (char *)malloc(1);
    if (!set_clause) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    set_clause[0] = '\0';

    char *qtable = cwist_orm_quote_identifier(orm, table);
    if (!qtable) {
        free(set_clause);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    int idx = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)data) {
        char *literal = cwist_orm_json_to_sql_literal(orm, item);
        char *qid = cwist_orm_quote_identifier(orm, item->string);
        if (!literal || !qid) {
            free(literal);
            free(qid);
            free(qtable);
            free(set_clause);
            err.error.err_i16 = CWIST_ERROR_NOMEM;
            return err;
        }
        size_t need = strlen(set_clause) + strlen(qid) + strlen(literal) + 4;
        char *new_set = (char *)realloc(set_clause, need);
        if (!new_set) {
            free(literal);
            free(qid);
            free(qtable);
            free(set_clause);
            err.error.err_i16 = CWIST_ERROR_NOMEM;
            return err;
        }
        set_clause = new_set;
        if (idx > 0) strcat(set_clause, ",");
        strcat(set_clause, qid);
        strcat(set_clause, "=");
        strcat(set_clause, literal);
        free(literal);
        free(qid);
        idx++;
    }

    if (idx == 0) {
        free(qtable);
        free(set_clause);
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    size_t sql_len =
        strlen(qtable) + strlen(set_clause) + 32 + (where_clause ? strlen(where_clause) : 0);
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(qtable);
        free(set_clause);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (where_clause && *where_clause) {
        snprintf(sql, sql_len, "UPDATE %s SET %s WHERE %s;", qtable, set_clause, where_clause);
    } else {
        snprintf(sql, sql_len, "UPDATE %s SET %s;", qtable, set_clause);
    }
    free(qtable);
    free(set_clause);

    err = cwist_orm_exec(orm, sql);
    free(sql);
    return err;
}

/**
 * @brief Delete rows from a table.
 *
 * If @p where_clause is non-empty it is appended as a WHERE clause; a NULL
 * or empty clause deletes all rows.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_NOMEM on allocation failure, otherwise
 *         the error from cwist_orm_exec().
 */
cwist_error_t cwist_orm_delete(cwist_orm_t *orm, const char *table, const char *where_clause) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!orm || !table) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *qtable = cwist_orm_quote_identifier(orm, table);
    if (!qtable) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    size_t sql_len = strlen(qtable) + 32 + (where_clause ? strlen(where_clause) : 0);
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(qtable);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (where_clause && *where_clause) {
        snprintf(sql, sql_len, "DELETE FROM %s WHERE %s;", qtable, where_clause);
    } else {
        snprintf(sql, sql_len, "DELETE FROM %s;", qtable);
    }
    free(qtable);

    err = cwist_orm_exec(orm, sql);
    free(sql);
    return err;
}

/**
 * @brief Select rows from a table.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param columns      Raw comma-separated column list (not escaped).
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param result       [out] Receives a cJSON array of row objects; caller
 *                     must release with cJSON_Delete().
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_NOMEM on allocation failure, otherwise
 *         the error from cwist_orm_query().
 */
cwist_error_t cwist_orm_select(cwist_orm_t *orm, const char *table, const char *columns,
                               const char *where_clause, cJSON **result) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    *result = NULL;
    if (!orm || !table || !columns) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *qtable = cwist_orm_quote_identifier(orm, table);
    if (!qtable) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    size_t sql_len =
        strlen(columns) + strlen(qtable) + 32 + (where_clause ? strlen(where_clause) : 0);
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(qtable);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (where_clause && *where_clause) {
        snprintf(sql, sql_len, "SELECT %s FROM %s WHERE %s;", columns, qtable, where_clause);
    } else {
        snprintf(sql, sql_len, "SELECT %s FROM %s;", columns, qtable);
    }
    free(qtable);

    err = cwist_orm_query(orm, sql, result);
    free(sql);
    return err;
}

/* ------------------------------------------------------------------ */
/* _Generic helpers: RETURNING INSERT                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Insert a row and return a JSON value from the new row.
 *
 * Appends a dialect-specific RETURNING clause (or an equivalent
 * last-insert-id follow-up query) to the INSERT built from @p data, then
 * extracts the @p returning_col value from the first result row. For
 * MySQL/SQLite the returned value is the last insert id, not an arbitrary
 * column.
 *
 * @param orm           Active session.
 * @param table         Table name.
 * @param data          cJSON object with the column values to insert.
 * @param returning_col Column (or alias) of the value to return.
 * @param out           [out] Receives a duplicated cJSON value; caller must
 *                      release with cJSON_Delete().
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_NOMEM on allocation failure,
 *         CWIST_ERROR_PROTOCOL when the value is missing from the result.
 */
cwist_error_t cwist_orm_insert_returning_json(cwist_orm_t *orm, const char *table,
                                              const cJSON *data, const char *returning_col,
                                              cJSON **out) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (out) *out = NULL;
    if (!orm || !table || !data || !returning_col || !out) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *base = orm_build_insert_sql(orm, table, data);
    if (!base) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    char *qret = cwist_orm_quote_identifier(orm, returning_col);
    if (!qret) {
        free(base);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    size_t sql_len = strlen(base) + strlen(qret) + 16;
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(base);
        free(qret);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    switch (orm->dialect) {
        case CWIST_ORM_POSTGRES:
        case CWIST_ORM_MARIADB: snprintf(sql, sql_len, "%s RETURNING %s;", base, qret); break;
        case CWIST_ORM_MYSQL:
            snprintf(sql, sql_len, "%s; SELECT LAST_INSERT_ID() AS %s;", base, qret);
            break;
        case CWIST_ORM_SQLITE:
        default: snprintf(sql, sql_len, "%s; SELECT last_insert_rowid() AS %s;", base, qret); break;
    }
    free(base);
    free(qret);

    cJSON *rows = NULL;
    err = cwist_orm_query(orm, sql, &rows);
    free(sql);
    if (err.error.err_i16 != 0) {
        if (rows) cJSON_Delete(rows);
        return err;
    }

    cJSON *first = cJSON_GetArrayItem(rows, 0);
    if (!first) {
        cJSON_Delete(rows);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    cJSON *val = cJSON_GetObjectItemCaseSensitive(first, returning_col);
    if (!val) {
        cJSON_Delete(rows);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    *out = cJSON_Duplicate(val, 1);
    cJSON_Delete(rows);
    if (!*out) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Insert a row and return a column value as an int.
 *
 * @param orm           Active session.
 * @param table         Table name.
 * @param data          cJSON object with the column values to insert.
 * @param returning_col Column (or alias) of the value to return.
 * @param out           [out] Receives the value as an int.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_insert_returning_json() or CWIST_ERROR_PROTOCOL when
 *         the returned value is not a number.
 */
cwist_error_t cwist_orm_insert_returning_int(cwist_orm_t *orm, const char *table, const cJSON *data,
                                             const char *returning_col, int *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_insert_returning_json(orm, table, data, returning_col, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (int)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Insert a row and return a column value as a long.
 *
 * @param orm           Active session.
 * @param table         Table name.
 * @param data          cJSON object with the column values to insert.
 * @param returning_col Column (or alias) of the value to return.
 * @param out           [out] Receives the value as a long.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_insert_returning_json() or CWIST_ERROR_PROTOCOL when
 *         the returned value is not a number.
 */
cwist_error_t cwist_orm_insert_returning_long(cwist_orm_t *orm, const char *table,
                                              const cJSON *data, const char *returning_col,
                                              long *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_insert_returning_json(orm, table, data, returning_col, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (long)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Insert a row and return a column value as a long long.
 *
 * @param orm           Active session.
 * @param table         Table name.
 * @param data          cJSON object with the column values to insert.
 * @param returning_col Column (or alias) of the value to return.
 * @param out           [out] Receives the value as a long long.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_insert_returning_json() or CWIST_ERROR_PROTOCOL when
 *         the returned value is not a number.
 */
cwist_error_t cwist_orm_insert_returning_llong(cwist_orm_t *orm, const char *table,
                                               const cJSON *data, const char *returning_col,
                                               long long *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_insert_returning_json(orm, table, data, returning_col, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (long long)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/* ------------------------------------------------------------------ */
/* _Generic helpers: SELECT single scalar                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Select a single column value from the first matching row as JSON.
 *
 * Runs a `SELECT column FROM table [WHERE ...] LIMIT 1` query and
 * duplicates the column value from the first result row.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives a duplicated cJSON value; caller must
 *                     release with cJSON_Delete().
 * @return CWIST_SUCCESS on success; CWIST_ERROR_INVALID_PARAM for NULL
 *         arguments, CWIST_ERROR_NOMEM on allocation failure,
 *         CWIST_ERROR_PROTOCOL when no row or column value is returned.
 */
cwist_error_t cwist_orm_select_one_json(cwist_orm_t *orm, const char *table, const char *column,
                                        const char *where_clause, cJSON **out) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (out) *out = NULL;
    if (!orm || !table || !column || !out) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    char *qtable = cwist_orm_quote_identifier(orm, table);
    char *qcol = cwist_orm_quote_identifier(orm, column);
    if (!qtable || !qcol) {
        free(qtable);
        free(qcol);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    size_t sql_len = strlen(qcol) + strlen(qtable) + 48 + (where_clause ? strlen(where_clause) : 0);
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(qtable);
        free(qcol);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (where_clause && *where_clause) {
        snprintf(sql, sql_len, "SELECT %s FROM %s WHERE %s LIMIT 1;", qcol, qtable, where_clause);
    } else {
        snprintf(sql, sql_len, "SELECT %s FROM %s LIMIT 1;", qcol, qtable);
    }
    free(qtable);
    free(qcol);

    cJSON *rows = NULL;
    err = cwist_orm_query(orm, sql, &rows);
    free(sql);
    if (err.error.err_i16 != 0) {
        if (rows) cJSON_Delete(rows);
        return err;
    }

    cJSON *first = cJSON_GetArrayItem(rows, 0);
    if (!first) {
        cJSON_Delete(rows);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    cJSON *val = cJSON_GetObjectItemCaseSensitive(first, column);
    if (!val) {
        cJSON_Delete(rows);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    *out = cJSON_Duplicate(val, 1);
    cJSON_Delete(rows);
    if (!*out) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Select a single column value from the first matching row as an int.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives the value as an int.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_select_one_json() or CWIST_ERROR_PROTOCOL when the
 *         returned value is not a number.
 */
cwist_error_t cwist_orm_select_one_int(cwist_orm_t *orm, const char *table, const char *column,
                                       const char *where_clause, int *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_select_one_json(orm, table, column, where_clause, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (int)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Select a single column value from the first matching row as a long.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives the value as a long.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_select_one_json() or CWIST_ERROR_PROTOCOL when the
 *         returned value is not a number.
 */
cwist_error_t cwist_orm_select_one_long(cwist_orm_t *orm, const char *table, const char *column,
                                        const char *where_clause, long *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_select_one_json(orm, table, column, where_clause, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (long)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Select a single column value from the first matching row as a long long.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives the value as a long long.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_select_one_json() or CWIST_ERROR_PROTOCOL when the
 *         returned value is not a number.
 */
cwist_error_t cwist_orm_select_one_llong(cwist_orm_t *orm, const char *table, const char *column,
                                         const char *where_clause, long long *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_select_one_json(orm, table, column, where_clause, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = (long long)val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Select a single column value from the first matching row as a double.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives the value as a double.
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_select_one_json() or CWIST_ERROR_PROTOCOL when the
 *         returned value is not a number.
 */
cwist_error_t cwist_orm_select_one_double(cwist_orm_t *orm, const char *table, const char *column,
                                          const char *where_clause, double *out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_select_one_json(orm, table, column, where_clause, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsNumber(val)) {
        *out = val->valuedouble;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}

/**
 * @brief Select a single column value from the first matching row as a string.
 *
 * @param orm          Active session.
 * @param table        Table name.
 * @param column       Column name.
 * @param where_clause Optional raw SQL WHERE condition (not escaped).
 * @param out          [out] Receives a heap-allocated copy of the string;
 *                     caller must release with free().
 * @return CWIST_SUCCESS on success; otherwise the error from
 *         cwist_orm_select_one_json(), CWIST_ERROR_PROTOCOL when the
 *         returned value is not a string, or CWIST_ERROR_NOMEM when the
 *         string copy cannot be allocated.
 */
cwist_error_t cwist_orm_select_one_string(cwist_orm_t *orm, const char *table, const char *column,
                                          const char *where_clause, char **out) {
    cJSON *val = NULL;
    cwist_error_t err = cwist_orm_select_one_json(orm, table, column, where_clause, &val);
    if (err.error.err_i16 != 0) return err;
    if (cJSON_IsString(val)) {
        *out = strdup(val->valuestring);
        if (!*out) err.error.err_i16 = CWIST_ERROR_NOMEM;
    } else {
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    }
    cJSON_Delete(val);
    return err;
}
