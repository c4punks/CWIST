/**
 * @file pg.h
 * @brief PostgreSQL client integration using libpq.
 */

#ifndef CWIST_PG_H
#define CWIST_PG_H

#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_pg cwist_pg_t;

/**
 * @brief Connect to a PostgreSQL database using a connection string or URI.
 *
 * @param conninfo Connection string (e.g. "host=localhost dbname=test user=postgres")
 *                 or URI (e.g. "postgresql://user:pass@host:port/dbname").
 * @return Opaque connection handle, or NULL on allocation/connection failure.
 */
cwist_pg_t *cwist_pg_connect(const char *conninfo);

/**
 * @brief Disconnect and release PostgreSQL connection resources.
 *
 * @param pg Connection handle created with cwist_pg_connect().
 */
void cwist_pg_disconnect(cwist_pg_t *pg);

/**
 * @brief Check if the PostgreSQL connection is active and healthy.
 *
 * @param pg Connection handle.
 * @return true if connected and ready; false otherwise.
 */
bool cwist_pg_is_connected(cwist_pg_t *pg);

/**
 * @brief Execute a raw SQL statement (DDL/DML) without result rows.
 *
 * @param pg  Active connection.
 * @param sql Null-terminated SQL statement.
 * @return CWIST_SUCCESS on success, error code on failure.
 */
cwist_error_t cwist_pg_exec(cwist_pg_t *pg, const char *sql);

/**
 * @brief Execute a parameterized SQL query preventing SQL injection.
 *
 * Returns rows as a cJSON array of objects where each object represents a row
 * mapping column names to string values.
 *
 * @param pg           Active connection.
 * @param sql          SQL query with $1, $2, ... placeholders.
 * @param n_params     Number of parameters.
 * @param param_values Array of null-terminated string parameter values.
 * @param out_rows     [out] Receives cJSON array of row objects (caller deletes).
 * @return CWIST_SUCCESS on success, error code on failure.
 */
cwist_error_t cwist_pg_exec_params(cwist_pg_t *pg, const char *sql, int n_params,
                                   const char *const *param_values, cJSON **out_rows);

/**
 * @brief Begin a transaction.
 */
cwist_error_t cwist_pg_begin(cwist_pg_t *pg);

/**
 * @brief Commit the current transaction.
 */
cwist_error_t cwist_pg_commit(cwist_pg_t *pg);

/**
 * @brief Roll back the current transaction.
 */
cwist_error_t cwist_pg_rollback(cwist_pg_t *pg);

/**
 * @brief Get the last error message from the PostgreSQL backend.
 *
 * @param pg Connection handle.
 * @return Error message string, or empty string if no error.
 */
const char *cwist_pg_last_error(cwist_pg_t *pg);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_PG_H */
