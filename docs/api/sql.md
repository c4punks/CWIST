# Database API

*Header:* `<cwist/core/db/sql.h>`

Wrapper for SQLite3 database operations.

### `cwist_db_open`
```c
cwist_error_t cwist_db_open(cwist_db **db, const char *path);
```
Opens a connection to a SQLite database file.

### `cwist_db_open_memory`
```c
cwist_error_t cwist_db_open_memory(cwist_db **db, const void *buf, size_t len, int readonly);
```
Opens a database from an in-memory SQLite image (e.g. a blob fetched by a WASM
or edge host) via `sqlite3_deserialize`. The image is copied, so the caller
keeps ownership of `buf`. `readonly != 0` rejects writes with `SQLITE_READONLY`.

### `cwist_db_serialize`
```c
cwist_error_t cwist_db_serialize(cwist_db *db, void **out, size_t *out_len);
```
Serializes the database into a freshly allocated image buffer (free with
`cwist_free()`), suitable for persisting back to a blob or for handing to
`cwist_db_open_memory()`.

### `cwist_db_exec`
```c
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql);
```
Executes a non-query SQL command (INSERT, UPDATE, DELETE). Returns `err_i16 = -1`
when called with a NULL handle or SQL pointer so callers can safely guard inputs.

### `cwist_db_query`
```c
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result);
```
Executes a SELECT query. `result` is populated with a cJSON Array of Objects on
success and reset to `NULL` if validation or SQLite execution fails.

Integrate the handle with the framework via `cwist_app_use_db(app, "app.db");` — every incoming `cwist_http_request` then exposes the pointer at `req->db`.

## Schema Migrations

*Header:* `<cwist/core/db/migrate.h>`

### Return codes

| Constant | Value | Meaning |
|---|---|---|
| `CWIST_MIGRATE_OK` | `0` | Success |
| `CWIST_MIGRATE_ERR_GENERIC` | `-1` | General failure |
| `CWIST_MIGRATE_ERR_SQL` | `-2` | SQLite error |
| `CWIST_MIGRATE_ERR_ARGS` | `-3` | NULL or invalid arguments |

### `cwist_migration_t`
```c
typedef struct cwist_migration_t {
    int         version;   /* monotonically increasing integer */
    const char *name;      /* human-readable label */
    const char *up_sql;    /* forward SQL; multiple statements separated by ';' */
    const char *down_sql;  /* rollback SQL; NULL means irreversible */
} cwist_migration_t;
```

### `cwist_migrate_up`
```c
int cwist_migrate_up(sqlite3 *db, const cwist_migration_t *migrations, int count);
```
Applies all migrations whose version is greater than the current schema version, in ascending version order. Each migration runs inside its own transaction; on failure the transaction is rolled back and the error code is returned. The `migrations` array need not be sorted.

### `cwist_migrate_down`
```c
int cwist_migrate_down(sqlite3 *db, const cwist_migration_t *migrations, int count, int steps);
```
Rolls back the most recent `steps` applied migrations (pass `0` to roll back all). Skips migrations whose `down_sql` is `NULL`. Each rollback runs inside a transaction.

### `cwist_migrate_version`
```c
int cwist_migrate_version(sqlite3 *db);
```
Returns the highest version number applied so far, `0` if no migrations have been applied, or `-1` on error. Useful at startup to guard conditional migration logic.
