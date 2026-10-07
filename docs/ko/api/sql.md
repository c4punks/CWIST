# 데이터베이스 API

*헤더:* `<cwist/core/db/sql.h>`

SQLite3 데이터베이스 작업을 위한 래퍼입니다.

### `cwist_db_open`
```c
cwist_error_t cwist_db_open(cwist_db **db, const char *path);
```
SQLite 데이터베이스 파일에 연결합니다.

### `cwist_db_open_memory`
```c
cwist_error_t cwist_db_open_memory(cwist_db **db, const void *buf, size_t len, int readonly);
```
메모리에 있는 SQLite 이미지(예: WASM이나 엣지 호스트가 가져온 blob)에서
`sqlite3_deserialize`로 데이터베이스를 엽니다. 이미지는 복사되므로 `buf`의 소유권은 호출자에게
남습니다. `readonly != 0`이면 쓰기를 `SQLITE_READONLY`로 거부합니다.

### `cwist_db_serialize`
```c
cwist_error_t cwist_db_serialize(cwist_db *db, void **out, size_t *out_len);
```
데이터베이스를 새로 할당한 이미지 버퍼에 직렬화합니다 (`cwist_free()`로 해제). blob으로 다시
저장하거나 `cwist_db_open_memory()`에 넘기기에 알맞습니다.

### `cwist_db_exec`
```c
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql);
```
쿼리가 아닌 SQL 명령(INSERT, UPDATE, DELETE)을 실행합니다. NULL 핸들이나 SQL 포인터로
호출하면 `err_i16 = -1`을 반환하므로, 호출자가 입력을 안전하게 검사할 수 있습니다.

### `cwist_db_query`
```c
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result);
```
SELECT 쿼리를 실행합니다. 성공하면 `result`에 객체들의 cJSON 배열이 채워지고, 검증이나
SQLite 실행이 실패하면 `NULL`로 재설정됩니다.

핸들은 `cwist_app_use_db(app, "app.db");`로 프레임워크와 연동하세요. 그러면 들어오는 모든
`cwist_http_request`가 `req->db`에서 이 포인터를 노출합니다.

## 스키마 마이그레이션

*헤더:* `<cwist/core/db/migrate.h>`

### 반환 코드

| 상수 | 값 | 의미 |
|---|---|---|
| `CWIST_MIGRATE_OK` | `0` | 성공 |
| `CWIST_MIGRATE_ERR_GENERIC` | `-1` | 일반 실패 |
| `CWIST_MIGRATE_ERR_SQL` | `-2` | SQLite 에러 |
| `CWIST_MIGRATE_ERR_ARGS` | `-3` | NULL 또는 잘못된 인자 |

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
현재 스키마 버전보다 높은 버전의 마이그레이션을 버전 오름차순으로 모두 적용합니다. 각 마이그레이션은 자체 트랜잭션 안에서 실행되며, 실패하면 트랜잭션을 롤백하고 에러 코드를 반환합니다. `migrations` 배열은 정렬되어 있지 않아도 됩니다.

### `cwist_migrate_down`
```c
int cwist_migrate_down(sqlite3 *db, const cwist_migration_t *migrations, int count, int steps);
```
가장 최근에 적용한 마이그레이션 `steps`개를 롤백합니다 (`0`을 넘기면 전부 롤백). `down_sql`이 `NULL`인 마이그레이션은 건너뜁니다. 각 롤백은 트랜잭션 안에서 실행됩니다.

### `cwist_migrate_version`
```c
int cwist_migrate_version(sqlite3 *db);
```
지금까지 적용된 가장 높은 버전 번호를 반환합니다. 적용된 마이그레이션이 없으면 `0`, 에러면 `-1`입니다. 시작 시 조건부 마이그레이션 로직을 막는 데 유용합니다.
