/**
 * @file test_migrate.c
 * @brief Unit tests for the DB migration and db_crypt modules.
 */

#include <cwist/core/db/migrate.h>
#include <cwist/security/db_crypt/db_crypt.h>
#include <sqlite3.h>
#include <openssl/hmac.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define TEST_DB_CRYPT_AUTH_TAG_LEN 32

/* -------------------------------------------------------------------------
 * Migration tests
 * ---------------------------------------------------------------------- */

static const cwist_migration_t migrations[] = {
    {1, "create_users", "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);",
     "DROP TABLE IF EXISTS users;"},
    {
        2, "add_email", "ALTER TABLE users ADD COLUMN email TEXT;",
        NULL /* SQLite doesn't support DROP COLUMN in old versions */
    },
    {3, "create_sessions", "CREATE TABLE sessions (id TEXT PRIMARY KEY, user_id INTEGER);",
     "DROP TABLE IF EXISTS sessions;"},
};
static const int N_MIGRATIONS = 3;

static void test_migrate_up_and_version(void) {
    printf("Testing migrate up and version...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_version(db) == 0);

    int rc = cwist_migrate_up(db, migrations, N_MIGRATIONS);
    assert(rc == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    /* Verify tables exist. */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db,
                              "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                              "('users','sessions');",
                              -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_idempotent(void) {
    printf("Testing migrate up is idempotent...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_down_one(void) {
    printf("Testing migrate down 1 step...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    /* Roll back 1 step: removes sessions table. */
    int rc = cwist_migrate_down(db, migrations, N_MIGRATIONS, 1);
    assert(rc == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 2);

    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(
               db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='sessions';", -1,
               &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0); /* sessions table gone */
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_down_all(void) {
    printf("Testing migrate down all steps...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);

    int rc = cwist_migrate_down(db, migrations, N_MIGRATIONS, 0);
    assert(rc == CWIST_MIGRATE_ERR_GENERIC);

    /* v3 is rolled back, then the irreversible v2 stops the rollback. */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(
               db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='sessions';", -1,
               &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);
    assert(sqlite3_prepare_v2(
               db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='users';", -1,
               &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    /* The irreversible v2 must still be recorded as applied. */
    assert(cwist_migrate_version(db) == 2);

    /* Re-upping must not re-run v2's up_sql on the existing column. */
    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_down_steps_skip_irreversible(void) {
    printf("Testing migrate down does not spend a step on an irreversible migration...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    /* v3 is rolled back, then the irreversible v2 stops the second step. */
    assert(cwist_migrate_down(db, migrations, N_MIGRATIONS, 2) == CWIST_MIGRATE_ERR_GENERIC);
    assert(cwist_migrate_version(db) == 2);

    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db,
                              "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                              "('users','sessions');",
                              -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_history_failure_rolls_back(void) {
    printf("Testing migration history failures roll back schema changes...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(cwist_migrate_version(db) == 0);
    assert(sqlite3_exec(db,
                        "CREATE TRIGGER reject_history_insert BEFORE INSERT ON _cwist_migrations "
                        "BEGIN SELECT RAISE(FAIL, 'history unavailable'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);

    const cwist_migration_t migration = {
        1, "create_items", "CREATE TABLE items (id INTEGER PRIMARY KEY);", "DROP TABLE items;"};
    assert(cwist_migrate_up(db, &migration, 1) == CWIST_MIGRATE_ERR_SQL);
    assert(cwist_migrate_version(db) == 0);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE name='items';", -1,
                              &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    assert(sqlite3_exec(db, "DROP TRIGGER reject_history_insert;", NULL, NULL, NULL) == SQLITE_OK);
    assert(cwist_migrate_up(db, &migration, 1) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 1);
    assert(sqlite3_exec(db,
                        "CREATE TRIGGER reject_history_delete BEFORE DELETE ON _cwist_migrations "
                        "BEGIN SELECT RAISE(FAIL, 'history unavailable'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(cwist_migrate_down(db, &migration, 1, 1) == CWIST_MIGRATE_ERR_SQL);
    assert(cwist_migrate_version(db) == 1);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE name='items';", -1,
                              &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    assert(sqlite3_exec(db, "DROP TRIGGER reject_history_delete;", NULL, NULL, NULL) == SQLITE_OK);
    assert(cwist_migrate_down(db, &migration, 1, 1) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 0);
    sqlite3_close(db);
    printf("  Passed.\n");
}

/* -------------------------------------------------------------------------
 * db_crypt tests
 * ---------------------------------------------------------------------- */

static void test_db_crypt_roundtrip(void) {
    printf("Testing db_crypt seal/open roundtrip...\n");

    cwist_db_crypt_ctx_t ctx;
    /* Fill KEK with a deterministic test value. */
    memset(ctx.kek, 0xAB, CWIST_DB_CRYPT_KEY_LEN);

    const unsigned char plaintext[] = "Hello, SQLite world! This is a test database blob.";
    size_t pt_len = sizeof(plaintext) - 1;

    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(&ctx, plaintext, pt_len, &blob_len);
    assert(blob != NULL);
    assert(blob_len > pt_len);
    assert(blob[4] == 0x02);

    size_t out_len = 0;
    unsigned char *recovered = cwist_db_crypt_open(&ctx, blob, blob_len, &out_len);
    assert(recovered != NULL);
    assert(out_len == pt_len);
    assert(memcmp(recovered, plaintext, pt_len) == 0);

    free(blob);
    free(recovered);
    printf("  Passed.\n");
}

static void test_db_crypt_wrong_kek(void) {
    printf("Testing db_crypt rejects wrong KEK...\n");

    cwist_db_crypt_ctx_t ctx_enc, ctx_dec;
    memset(ctx_enc.kek, 0x11, CWIST_DB_CRYPT_KEY_LEN);
    memset(ctx_dec.kek, 0x22, CWIST_DB_CRYPT_KEY_LEN); /* different */

    const unsigned char data[] = "secret data";
    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(&ctx_enc, data, sizeof(data) - 1, &blob_len);
    assert(blob != NULL);

    size_t out_len = 0;
    unsigned char *recovered = cwist_db_crypt_open(&ctx_dec, blob, blob_len, &out_len);
    /* With wrong KEK the DEK will decrypt to garbage; decryption should fail
     * or return incorrect data. Either way we must not get the original back. */
    if (recovered) {
        assert(out_len != sizeof(data) - 1 || memcmp(recovered, data, sizeof(data) - 1) != 0);
        free(recovered);
    }
    free(blob);
    printf("  Passed.\n");
}

static void test_db_crypt_rejects_tampering(void) {
    printf("Testing db_crypt rejects authenticated blob tampering...\n");

    cwist_db_crypt_ctx_t ctx;
    memset(ctx.kek, 0x42, sizeof(ctx.kek));
    unsigned char plaintext[64];
    for (size_t i = 0; i < sizeof(plaintext); i++) plaintext[i] = (unsigned char)i;

    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(&ctx, plaintext, sizeof(plaintext), &blob_len);
    assert(blob != NULL);

    blob[CWIST_DB_CRYPT_HDR_LEN] ^= 1;
    size_t out_len = 0;
    unsigned char *opened = cwist_db_crypt_open(&ctx, blob, blob_len, &out_len);
    assert(opened == NULL);

    blob[CWIST_DB_CRYPT_HDR_LEN] ^= 1;
    blob[blob_len - 1] ^= 1;
    opened = cwist_db_crypt_open(&ctx, blob, blob_len, &out_len);
    assert(opened == NULL);

    assert(cwist_db_crypt_open_legacy(&ctx, blob, blob_len, &out_len) == NULL);
    free(blob);
    printf("  Passed.\n");
}

static void test_db_crypt_length_guards(void) {
    printf("Testing db_crypt length guards...\n");

    cwist_db_crypt_ctx_t ctx;
    memset(ctx.kek, 0x5A, sizeof(ctx.kek));
    const unsigned char plaintext[] = "length checked";
    size_t blob_len = 0;
    assert(cwist_db_crypt_seal(&ctx, plaintext, (size_t)INT_MAX + 1, &blob_len) == NULL);

    unsigned char *blob = cwist_db_crypt_seal(&ctx, plaintext, sizeof(plaintext) - 1, &blob_len);
    assert(blob != NULL);
    size_t pt_len_offset = CWIST_DB_CRYPT_HDR_LEN - 8;
    for (int i = 0; i < 8; i++) blob[pt_len_offset + (size_t)i] = 0;
    unsigned int tag_len = 0;
    assert(HMAC(EVP_sha256(), ctx.kek, CWIST_DB_CRYPT_KEY_LEN, blob,
                blob_len - TEST_DB_CRYPT_AUTH_TAG_LEN,
                blob + blob_len - TEST_DB_CRYPT_AUTH_TAG_LEN, &tag_len) != NULL);
    assert(tag_len == TEST_DB_CRYPT_AUTH_TAG_LEN);

    size_t out_len = 0;
    assert(cwist_db_crypt_open(&ctx, blob, blob_len, &out_len) == NULL);
    free(blob);
    printf("  Passed.\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== Migration tests ===\n");
    test_migrate_up_and_version();
    test_migrate_idempotent();
    test_migrate_down_one();
    test_migrate_down_all();
    test_migrate_down_steps_skip_irreversible();
    test_migrate_history_failure_rolls_back();

    printf("=== DB Crypt tests ===\n");
    test_db_crypt_roundtrip();
    test_db_crypt_wrong_kek();
    test_db_crypt_rejects_tampering();
    test_db_crypt_length_guards();

    printf("\nAll tests passed.\n");
    return 0;
}
