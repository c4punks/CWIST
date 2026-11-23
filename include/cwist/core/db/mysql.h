/**
 * @file mysql.h
 * @brief MySQL and MariaDB client integration.
 */

#ifndef CWIST_MYSQL_H
#define CWIST_MYSQL_H

#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_mysql cwist_mysql_t;

/**
 * @brief Connect to a MySQL or MariaDB database using a connection string or URI.
 *
 * @param conninfo Connection string (e.g. "host=localhost dbname=test user=root password=secret port=3306")
 *                 or URI (e.g. "mysql://user:pass@host:port/dbname").
 * @return Opaque connection handle, or NULL on allocation/connection failure.
 */
cwist_mysql_t *cwist_mysql_connect(const char *conninfo);

/**
 * @brief Disconnect and release MySQL/MariaDB connection resources.
 *
 * @param mysql Connection handle created with cwist_mysql_connect().
 */
void cwist_mysql_disconnect(cwist_mysql_t *mysql);

/**
 * @brief Check if the MySQL/MariaDB connection is active and healthy.
 *
 * @param mysql Connection handle.
 * @return true if connected and ready; false otherwise.
 */
bool cwist_mysql_is_connected(cwist_mysql_t *mysql);

/**
 * @brief Execute a raw SQL statement (DDL/DML) without result rows.
 *
 * @param mysql Active connection.
 * @param sql   Null-terminated SQL statement.
 * @return CWIST_SUCCESS on success, error code on failure.
 */
cwist_error_t cwist_mysql_exec(cwist_mysql_t *mysql, const char *sql);

/**
 * @brief Execute a parameterized SQL query preventing SQL injection.
 *
 * Returns rows as a cJSON array of objects where each object represents a row
 * mapping column names to string values.
 *
 * @param mysql        Active connection.
 * @param sql          SQL query with ? placeholders.
 * @param n_params     Number of parameters.
 * @param param_values Array of null-terminated string parameter values.
 * @param out_rows     [out] Receives cJSON array of row objects (caller deletes).
 * @return CWIST_SUCCESS on success, error code on failure.
 */
cwist_error_t cwist_mysql_exec_params(cwist_mysql_t *mysql, const char *sql, int n_params,
                                      const char *const *param_values, cJSON **out_rows);

/**
 * @brief Begin a transaction.
 */
cwist_error_t cwist_mysql_begin(cwist_mysql_t *mysql);

/**
 * @brief Commit the current transaction.
 */
cwist_error_t cwist_mysql_commit(cwist_mysql_t *mysql);

/**
 * @brief Roll back the current transaction.
 */
cwist_error_t cwist_mysql_rollback(cwist_mysql_t *mysql);

/**
 * @brief Get the last error message from the MySQL backend.
 *
 * @param mysql Connection handle.
 * @return Error message string, or empty string if no error.
 */
const char *cwist_mysql_last_error(cwist_mysql_t *mysql);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_MYSQL_H */
