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

static void mysql_parse_kv_field(const char *conninfo, const char *key, char *out, size_t out_cap) {
    out[0] = '\0';
    const char *p = strstr(conninfo, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '=') p++;
    size_t i = 0;
    while (*p && *p != ' ' && *p != ';' && *p != '&' && i + 1 < out_cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
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

    if (strncmp(conninfo, "mysql://", 8) == 0 || strncmp(conninfo, "mariadb://", 10) == 0) {
        const char *p = strstr(conninfo, "://") + 3;
        const char *at = strchr(p, '@');
        const char *slash = strchr(p, '/');

        if (at) {
            const char *colon = strchr(p, ':');
            if (colon && colon < at) {
                size_t ulen = (size_t)(colon - p);
                if (ulen >= sizeof(user)) ulen = sizeof(user) - 1;
                memcpy(user, p, ulen);
                user[ulen] = '\0';

                size_t plen = (size_t)(at - (colon + 1));
                if (plen >= sizeof(password)) plen = sizeof(password) - 1;
                memcpy(password, colon + 1, plen);
                password[plen] = '\0';
            } else {
                size_t ulen = (size_t)(at - p);
                if (ulen >= sizeof(user)) ulen = sizeof(user) - 1;
                memcpy(user, p, ulen);
                user[ulen] = '\0';
            }
            p = at + 1;
        }

        const char *host_end = slash ? slash : p + strlen(p);
        const char *colon = strchr(p, ':');
        if (colon && colon < host_end) {
            size_t hlen = (size_t)(colon - p);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, p, hlen);
            host[hlen] = '\0';
            port = (unsigned int)strtoul(colon + 1, NULL, 10);
        } else {
            size_t hlen = (size_t)(host_end - p);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, p, hlen);
            host[hlen] = '\0';
        }

        if (slash && *(slash + 1)) {
            const char *db_end = strchr(slash + 1, '?');
            size_t dlen = db_end ? (size_t)(db_end - (slash + 1)) : strlen(slash + 1);
            if (dlen >= sizeof(dbname)) dlen = sizeof(dbname) - 1;
            memcpy(dbname, slash + 1, dlen);
            dbname[dlen] = '\0';
        }
    } else {
        mysql_parse_kv_field(conninfo, "host", host, sizeof(host));
        mysql_parse_kv_field(conninfo, "user", user, sizeof(user));
        mysql_parse_kv_field(conninfo, "password", password, sizeof(password));
        if (password[0] == '\0') {
            mysql_parse_kv_field(conninfo, "pass", password, sizeof(password));
        }
        mysql_parse_kv_field(conninfo, "dbname", dbname, sizeof(dbname));
        if (dbname[0] == '\0') {
            mysql_parse_kv_field(conninfo, "database", dbname, sizeof(dbname));
        }
        char port_str[16] = "";
        mysql_parse_kv_field(conninfo, "port", port_str, sizeof(port_str));
        if (port_str[0] != '\0') {
            port = (unsigned int)strtoul(port_str, NULL, 10);
        }
    }

    if (!mysql_real_connect(mysql->conn, host[0] ? host : NULL,
                            user[0] ? user : NULL,
                            password[0] ? password : NULL,
                            dbname[0] ? dbname : NULL,
                            port, NULL, 0)) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        mysql_close(mysql->conn);
        cwist_free(mysql);
        return NULL;
    }

    mysql_set_last_error(mysql, NULL);
    return mysql;
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
    }

    mysql_set_last_error(mysql, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
    return err;
}

cwist_error_t cwist_mysql_exec_params(cwist_mysql_t *mysql, const char *sql, int n_params,
                                      const char *const *param_values, cJSON **out_rows) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (out_rows) *out_rows = NULL;
    if (!mysql || !mysql->conn || !sql) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

    MYSQL_STMT *stmt = mysql_stmt_init(mysql->conn);
    if (!stmt) {
        mysql_set_last_error(mysql, mysql_error(mysql->conn));
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_set_last_error(mysql, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    if (n_params > 0 && param_values) {
        MYSQL_BIND *binds = (MYSQL_BIND *)cwist_alloc(sizeof(MYSQL_BIND) * (size_t)n_params);
        if (!binds) {
            mysql_stmt_close(stmt);
            err.error.err_i16 = CWIST_ERROR_NOMEM;
            return err;
        }
        memset(binds, 0, sizeof(MYSQL_BIND) * (size_t)n_params);

        for (int i = 0; i < n_params; i++) {
            if (!param_values[i]) {
                binds[i].buffer_type = MYSQL_TYPE_NULL;
            } else {
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = (void *)param_values[i];
                binds[i].buffer_length = (unsigned long)strlen(param_values[i]);
            }
        }

        if (mysql_stmt_bind_param(stmt, binds) != 0) {
            mysql_set_last_error(mysql, mysql_stmt_error(stmt));
            cwist_free(binds);
            mysql_stmt_close(stmt);
            err.error.err_i16 = CWIST_ERROR_PROTOCOL;
            return err;
        }
        cwist_free(binds);
    }

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_set_last_error(mysql, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        err.error.err_i16 = CWIST_ERROR_PROTOCOL;
        return err;
    }

    if (out_rows) {
        MYSQL_RES *meta = mysql_stmt_result_metadata(stmt);
        if (meta) {
            unsigned int num_fields = mysql_num_fields(meta);
            MYSQL_FIELD *fields = mysql_fetch_fields(meta);
            cJSON *rows = cJSON_CreateArray();

            MYSQL_BIND *result_binds = (MYSQL_BIND *)cwist_alloc(sizeof(MYSQL_BIND) * num_fields);
            char **buffers = (char **)cwist_alloc(sizeof(char *) * num_fields);
            unsigned long *lengths = (unsigned long *)cwist_alloc(sizeof(unsigned long) * num_fields);
            cwist_mysql_bool_t *is_null = (cwist_mysql_bool_t *)cwist_alloc(sizeof(cwist_mysql_bool_t) * num_fields);

            if (result_binds && buffers && lengths && is_null) {
                memset(result_binds, 0, sizeof(MYSQL_BIND) * num_fields);
                for (unsigned int j = 0; j < num_fields; j++) {
                    buffers[j] = (char *)cwist_alloc(4096);
                    result_binds[j].buffer_type = MYSQL_TYPE_STRING;
                    result_binds[j].buffer = buffers[j];
                    result_binds[j].buffer_length = 4096;
                    result_binds[j].length = &lengths[j];
                    result_binds[j].is_null = &is_null[j];
                }

                if (mysql_stmt_bind_result(stmt, result_binds) == 0 &&
                    mysql_stmt_store_result(stmt) == 0) {
                    while (mysql_stmt_fetch(stmt) == 0) {
                        cJSON *row = cJSON_CreateObject();
                        for (unsigned int j = 0; j < num_fields; j++) {
                            const char *fname = fields[j].name ? fields[j].name : "";
                            if (is_null[j]) {
                                cJSON_AddNullToObject(row, fname);
                            } else {
                                if (lengths[j] < 4096) {
                                    buffers[j][lengths[j]] = '\0';
                                } else {
                                    buffers[j][4095] = '\0';
                                }
                                cJSON_AddStringToObject(row, fname, buffers[j]);
                            }
                        }
                        cJSON_AddItemToArray(rows, row);
                    }
                }

                for (unsigned int j = 0; j < num_fields; j++) {
                    cwist_free(buffers[j]);
                }
            }

            cwist_free(result_binds);
            cwist_free(buffers);
            cwist_free(lengths);
            cwist_free(is_null);
            mysql_free_result(meta);
            *out_rows = rows;
        }
    }

    mysql_stmt_close(stmt);
    mysql_set_last_error(mysql, NULL);
    err.error.err_i16 = CWIST_SUCCESS;
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
    (void)mysql; (void)sql;
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_mysql_exec_params(cwist_mysql_t *mysql, const char *sql, int n_params,
                                      const char *const *param_values, cJSON **out_rows) {
    (void)mysql; (void)sql; (void)n_params; (void)param_values;
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
