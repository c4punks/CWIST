/**
 * @file mysql.c
 * @brief MySQL and MariaDB client integration.
 */

#include <cwist/core/db/mysql.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CWIST_HAS_MYSQL)
#if defined(__has_include)
#if __has_include(<mysql/mysql.h>)
#define CWIST_INTERNAL_MYSQL 1
#include <mysql/mysql.h>
#elif __has_include(<mysql.h>)
#define CWIST_INTERNAL_MYSQL 1
#include <mysql.h>
#elif __has_include(<mariadb/mysql.h>)
#define CWIST_INTERNAL_MYSQL 1
#include <mariadb/mysql.h>
#endif
#else
#define CWIST_INTERNAL_MYSQL 1
#include <mysql/mysql.h>
#endif
#endif

#if defined(CWIST_INTERNAL_MYSQL)

typedef __typeof__(*((MYSQL_BIND *)0)->is_null) cwist_mysql_bool_t;

struct cwist_mysql {
    MYSQL *conn;
    char last_error[512];
};

static void mysql_set_last_error(cwist_mysql_t *mysql, const char *msg) {
    if (!mysql) return;
    if (msg) {
        snprintf(mysql->last_error, sizeof(mysql->last_error), "%s", msg);
    } else {
        mysql->last_error[0] = '\0';
    }
}

static bool mysql_parse_kv_field(const char *conninfo, const char *key, char *out, size_t out_cap,
                                 bool *present) {
    size_t key_len = strlen(key);
    out[0] = '\0';
    *present = false;
    const char *p = conninfo;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '&') p++;
        const char *key_start = p;
        while (*p && *p != ' ' && *p != '=' && *p != ';' && *p != '&') p++;
        const char *key_end = p;
        while (*p == ' ') p++;
        if (*p != '=') {
            while (*p && *p != ' ' && *p != ';' && *p != '&') p++;
            continue;
        }
        p++;
        while (*p == ' ') p++;
        char quote = (*p == '\'' || *p == '"') ? *p++ : 0;
        const char *value_start = p;
        while (*p && (quote ? *p != quote : (*p != ' ' && *p != ';' && *p != '&'))) p++;
        size_t value_len = (size_t)(p - value_start);
        if (quote) {
            if (*p != quote) return false;
            p++;
        }
        while (*p == ' ') p++;
        if (*p && *p != ';' && *p != '&') return false;
        if ((size_t)(key_end - key_start) == key_len && strncmp(key_start, key, key_len) == 0) {
            if (value_len >= out_cap) return false;
            memcpy(out, value_start, value_len);
            out[value_len] = '\0';
            *present = true;
            return true;
        }
        if (quote && *p == quote) p++;
        while (*p && *p != ' ' && *p != ';' && *p != '&') p++;
    }
    return true;
}

static int mysql_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool mysql_uri_component(char *out, size_t out_cap, const char *start, size_t len) {
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c == '%') {
            if (i + 2 >= len) return false;
            int hi = mysql_hex_digit((unsigned char)start[i + 1]);
            int lo = mysql_hex_digit((unsigned char)start[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (out_len + 1 >= out_cap) return false;
        out[out_len++] = (char)c;
    }
    out[out_len] = '\0';
    return true;
}

static bool mysql_parse_uint(const char *start, size_t len, unsigned int max, unsigned int *out) {
    if (len == 0) return false;
    unsigned int value = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c < '0' || c > '9') return false;
        unsigned int digit = c - '0';
        if (value > (max - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == 0 || value > max) return false;
    *out = value;
    return true;
}

cwist_mysql_t *cwist_mysql_connect(const char *conninfo) {
    if (!conninfo || conninfo[0] == '\0') return NULL;

    cwist_mysql_t *mysql = (cwist_mysql_t *)cwist_alloc(sizeof(*mysql));
    if (!mysql) return NULL;
    memset(mysql, 0, sizeof(*mysql));

    mysql->conn = mysql_init(NULL);
    if (!mysql->conn) {
        cwist_free(mysql);
        return NULL;
    }

    char host[256] = "127.0.0.1";
    char user[128] = "root";
    char password[128] = "";
    char dbname[128] = "";
    unsigned int port = 3306;
    unsigned int connect_timeout = 5;
    unsigned int read_timeout = 30;
    unsigned int write_timeout = 30;
    bool present;

    if (strncmp(conninfo, "mysql://", 8) == 0 || strncmp(conninfo, "mariadb://", 10) == 0) {
        const char *p = strstr(conninfo, "://") + 3;
        const char *authority_end = p + strcspn(p, "/?");
        const char *slash = *authority_end == '/' ? authority_end : NULL;
        const char *at = memchr(p, '@', (size_t)(authority_end - p));
        if (at) {
            const char *colon = memchr(p, ':', (size_t)(at - p));
            if (colon) {
                if (!mysql_uri_component(user, sizeof(user), p, (size_t)(colon - p)) ||
                    !mysql_uri_component(password, sizeof(password), colon + 1,
                                         (size_t)(at - colon - 1)))
                    goto invalid;
            } else if (!mysql_uri_component(user, sizeof(user), p, (size_t)(at - p))) {
                goto invalid;
            }
            p = at + 1;
        }

        if (p < authority_end && *p == '[') {
            const char *close = memchr(p + 1, ']', (size_t)(authority_end - p - 1));
            if (!close || !mysql_uri_component(host, sizeof(host), p + 1, (size_t)(close - p - 1)))
                goto invalid;
            if (close + 1 < authority_end) {
                if (close[1] != ':' ||
                    !mysql_parse_uint(close + 2, (size_t)(authority_end - close - 2), 65535, &port))
                    goto invalid;
            }
        } else {
            const char *colon = memchr(p, ':', (size_t)(authority_end - p));
            const char *host_end = colon ? colon : authority_end;
            if (colon &&
                (memchr(colon + 1, ':', (size_t)(authority_end - colon - 1)) ||
                 !mysql_parse_uint(colon + 1, (size_t)(authority_end - colon - 1), 65535, &port)))
                goto invalid;
            if (!mysql_uri_component(host, sizeof(host), p, (size_t)(host_end - p))) goto invalid;
        }
        if (slash && slash[1]) {
            const char *db_end = strchr(slash + 1, '?');
            size_t db_len = db_end ? (size_t)(db_end - slash - 1) : strlen(slash + 1);
            if (!mysql_uri_component(dbname, sizeof(dbname), slash + 1, db_len)) goto invalid;
        }
    } else {
        if (!mysql_parse_kv_field(conninfo, "host", host, sizeof(host), &present) ||
            !mysql_parse_kv_field(conninfo, "user", user, sizeof(user), &present) ||
            !mysql_parse_kv_field(conninfo, "password", password, sizeof(password), &present))
            goto invalid;
        if (password[0] == '\0' &&
            !mysql_parse_kv_field(conninfo, "pass", password, sizeof(password), &present))
            goto invalid;
        if (!mysql_parse_kv_field(conninfo, "dbname", dbname, sizeof(dbname), &present))
            goto invalid;
        if (dbname[0] == '\0' &&
            !mysql_parse_kv_field(conninfo, "database", dbname, sizeof(dbname), &present))
            goto invalid;
        char number[16];
        if (!mysql_parse_kv_field(conninfo, "port", number, sizeof(number), &present)) goto invalid;
        if (present && !mysql_parse_uint(number, strlen(number), 65535, &port)) goto invalid;
        if (!mysql_parse_kv_field(conninfo, "connect_timeout", number, sizeof(number), &present))
            goto invalid;
        if (present && !mysql_parse_uint(number, strlen(number), 86400, &connect_timeout))
            goto invalid;
        if (!mysql_parse_kv_field(conninfo, "read_timeout", number, sizeof(number), &present))
            goto invalid;
        if (present && !mysql_parse_uint(number, strlen(number), 86400, &read_timeout))
            goto invalid;
        if (!mysql_parse_kv_field(conninfo, "write_timeout", number, sizeof(number), &present))
            goto invalid;
        if (present && !mysql_parse_uint(number, strlen(number), 86400, &write_timeout))
            goto invalid;
    }

    if (mysql_options(mysql->conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout) != 0 ||
        mysql_options(mysql->conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout) != 0 ||
        mysql_options(mysql->conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout) != 0)
        goto invalid;

    if (!mysql_real_connect(mysql->conn, host[0] ? host : NULL, user[0] ? user : NULL,
                            password[0] ? password : NULL, dbname[0] ? dbname : NULL, port, NULL,
                            0)) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        goto invalid;
    }

    mysql_set_last_error(mysql, NULL);
    return mysql;

invalid:
    mysql_close(mysql->conn);
    cwist_free(mysql);
    return NULL;
}

void cwist_mysql_disconnect(cwist_mysql_t *mysql) {
    if (!mysql) return;
    if (mysql->conn) {
        mysql_close(mysql->conn);
        mysql->conn = NULL;
    }
    cwist_free(mysql);
}

bool cwist_mysql_is_connected(cwist_mysql_t *mysql) {
    if (!mysql || !mysql->conn) return false;
    return mysql_ping(mysql->conn) == 0;
}

cwist_error_t cwist_mysql_exec(cwist_mysql_t *mysql, const char *sql) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!mysql || !mysql->conn || !sql) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    if (mysql_query(mysql->conn, sql) != 0) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    MYSQL_RES *res = mysql_store_result(mysql->conn);
    if (res) {
        mysql_free_result(res);
    } else if (mysql_field_count(mysql->conn) != 0 && mysql_errno(mysql->conn) != 0) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        err.error.err_i16 = CWIST_ERROR_IO;
        return err;
    }

    mysql_set_last_error(mysql, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

cwist_error_t cwist_mysql_exec_params(cwist_mysql_t *mysql, const char *sql, int n_params,
                                      const char *const *param_values, cJSON **out_rows) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    MYSQL_STMT *stmt = NULL;
    MYSQL_RES *meta = NULL;
    MYSQL_BIND *param_binds = NULL;
    MYSQL_BIND *result_binds = NULL;
    char **buffers = NULL;
    size_t *capacities = NULL;
    unsigned long *lengths = NULL;
    cwist_mysql_bool_t *is_null = NULL;
    cJSON *rows = NULL;
    cJSON *row = NULL;
    unsigned int num_fields = 0;

    if (out_rows) *out_rows = NULL;
    if (!mysql || !mysql->conn || !sql || n_params < 0 || (n_params > 0 && !param_values)) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    stmt = mysql_stmt_init(mysql->conn);
    if (!stmt) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto driver_error;

    if (n_params > 0) {
        param_binds = (MYSQL_BIND *)cwist_alloc(sizeof(MYSQL_BIND) * (size_t)n_params);
        if (!param_binds) goto oom;
        memset(param_binds, 0, sizeof(MYSQL_BIND) * (size_t)n_params);
        for (int i = 0; i < n_params; i++) {
            if (!param_values[i]) {
                param_binds[i].buffer_type = MYSQL_TYPE_NULL;
            } else {
                param_binds[i].buffer_type = MYSQL_TYPE_STRING;
                param_binds[i].buffer = (void *)param_values[i];
                param_binds[i].buffer_length = (unsigned long)strlen(param_values[i]);
            }
        }
        if (mysql_stmt_bind_param(stmt, param_binds) != 0) goto driver_error;
    }

    if (mysql_stmt_execute(stmt) != 0) goto driver_error;

    if (out_rows) {
        meta = mysql_stmt_result_metadata(stmt);
        if (!meta && mysql_stmt_field_count(stmt) != 0) goto driver_error;
        if (meta) {
            num_fields = mysql_num_fields(meta);
            MYSQL_FIELD *fields = mysql_fetch_fields(meta);
            rows = cJSON_CreateArray();
            result_binds = (MYSQL_BIND *)cwist_alloc(sizeof(MYSQL_BIND) * num_fields);
            buffers = (char **)cwist_alloc(sizeof(char *) * num_fields);
            capacities = (size_t *)cwist_alloc(sizeof(size_t) * num_fields);
            lengths = (unsigned long *)cwist_alloc(sizeof(unsigned long) * num_fields);
            is_null = (cwist_mysql_bool_t *)cwist_alloc(sizeof(cwist_mysql_bool_t) * num_fields);
            if (!rows || !result_binds || !buffers || !capacities || !lengths || !is_null) goto oom;
            memset(result_binds, 0, sizeof(MYSQL_BIND) * num_fields);
            memset(buffers, 0, sizeof(char *) * num_fields);
            for (unsigned int j = 0; j < num_fields; j++) {
                buffers[j] = (char *)cwist_alloc(4096);
                if (!buffers[j]) goto oom;
                capacities[j] = 4096;
                result_binds[j].buffer_type = MYSQL_TYPE_STRING;
                result_binds[j].buffer = buffers[j];
                result_binds[j].buffer_length = 4096;
                result_binds[j].length = &lengths[j];
                result_binds[j].is_null = &is_null[j];
            }
            if (mysql_stmt_bind_result(stmt, result_binds) != 0 ||
                mysql_stmt_store_result(stmt) != 0)
                goto driver_error;

            for (;;) {
                int fetch_rc = mysql_stmt_fetch(stmt);
                if (fetch_rc == MYSQL_NO_DATA) break;
                if (fetch_rc != 0 && fetch_rc != MYSQL_DATA_TRUNCATED) goto driver_error;
                bool resized = false;
                for (unsigned int j = 0; j < num_fields; j++) {
                    if (is_null[j] || lengths[j] < capacities[j]) continue;
                    size_t new_cap = (size_t)lengths[j] + 1;
                    char *new_buffer = (char *)cwist_alloc(new_cap);
                    if (!new_buffer) goto oom;
                    cwist_free(buffers[j]);
                    buffers[j] = new_buffer;
                    capacities[j] = new_cap;
                    result_binds[j].buffer = new_buffer;
                    result_binds[j].buffer_length = lengths[j] + 1;
                    if (mysql_stmt_fetch_column(stmt, &result_binds[j], j, 0) != 0)
                        goto driver_error;
                    resized = true;
                }
                if (resized && mysql_stmt_bind_result(stmt, result_binds) != 0) goto driver_error;

                row = cJSON_CreateObject();
                if (!row) goto oom;
                for (unsigned int j = 0; j < num_fields; j++) {
                    const char *fname = fields[j].name ? fields[j].name : "";
                    cJSON *cell;
                    if (is_null[j]) {
                        cell = cJSON_AddNullToObject(row, fname);
                    } else {
                        buffers[j][lengths[j]] = '\0';
                        cell = cJSON_AddStringToObject(row, fname, buffers[j]);
                    }
                    if (!cell) goto oom;
                }
                if (!cJSON_AddItemToArray(rows, row)) goto oom;
                row = NULL;
            }
            *out_rows = rows;
            rows = NULL;
        }
    }

    mysql_set_last_error(mysql, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    goto cleanup;

driver_error:
    mysql_set_last_error(mysql, mysql_stmt_error(stmt));
    err.error.err_i16 = CWIST_ERROR_PROTOCOL;
    goto cleanup;
oom:
    mysql_set_last_error(mysql, "out of memory");
    err.error.err_i16 = CWIST_ERROR_NOMEM;
cleanup:
    cJSON_Delete(row);
    cJSON_Delete(rows);
    if (buffers) {
        for (unsigned int j = 0; j < num_fields; j++) cwist_free(buffers[j]);
    }
    cwist_free(result_binds);
    cwist_free(buffers);
    cwist_free(capacities);
    cwist_free(lengths);
    cwist_free(is_null);
    if (meta) mysql_free_result(meta);
    cwist_free(param_binds);
    if (stmt) mysql_stmt_close(stmt);
    return err;
}

cwist_error_t cwist_mysql_begin(cwist_mysql_t *mysql) {
    return cwist_mysql_exec(mysql, "START TRANSACTION;");
}

cwist_error_t cwist_mysql_commit(cwist_mysql_t *mysql) {
    return cwist_mysql_exec(mysql, "COMMIT;");
}

cwist_error_t cwist_mysql_rollback(cwist_mysql_t *mysql) {
    return cwist_mysql_exec(mysql, "ROLLBACK;");
}

const char *cwist_mysql_last_error(cwist_mysql_t *mysql) {
    if (!mysql) return "";
    return mysql->last_error;
}

#else

/* Stubs when CWIST_HAS_MYSQL is not enabled or mysql client header is absent */
cwist_mysql_t *cwist_mysql_connect(const char *conninfo) {
    (void)conninfo;
    return NULL;
}

void cwist_mysql_disconnect(cwist_mysql_t *mysql) {
    (void)mysql;
}

bool cwist_mysql_is_connected(cwist_mysql_t *mysql) {
    (void)mysql;
    return false;
}

cwist_error_t cwist_mysql_exec(cwist_mysql_t *mysql, const char *sql) {
    (void)mysql;
    (void)sql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_mysql_exec_params(cwist_mysql_t *mysql, const char *sql, int n_params,
                                      const char *const *param_values, cJSON **out_rows) {
    (void)mysql;
    (void)sql;
    (void)n_params;
    (void)param_values;
    if (out_rows) *out_rows = NULL;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_mysql_begin(cwist_mysql_t *mysql) {
    (void)mysql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_mysql_commit(cwist_mysql_t *mysql) {
    (void)mysql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_mysql_rollback(cwist_mysql_t *mysql) {
    (void)mysql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

const char *cwist_mysql_last_error(cwist_mysql_t *mysql) {
    (void)mysql;
    return "mysql support is disabled";
}

#endif
