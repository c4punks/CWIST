#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char *error = NULL;

    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE VIRTUAL TABLE documents USING fts5(content);", NULL, NULL,
                        &error) == SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO documents(content) VALUES ('CWIST SQLite search');", NULL,
                        NULL, &error) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT content FROM documents WHERE documents MATCH 'CWIST';",
                              -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "CWIST SQLite search") == 0);
    assert(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
    sqlite3_free(error);
    assert(sqlite3_close(db) == SQLITE_OK);
    printf("test_sqlite_fts5: OK\n");
    return 0;
}
