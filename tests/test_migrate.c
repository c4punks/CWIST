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

static void test_migrate_extreme_versions(void) {
    printf("Testing migration ordering at integer boundaries...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    const cwist_migration_t extreme[] = {
        {INT_MAX, "last", "CREATE TABLE last_version (id INTEGER);", NULL},
        {1, "first", "CREATE TABLE first_version (id INTEGER);", NULL},
        {INT_MIN, "skipped", "CREATE TABLE skipped_version (id INTEGER);", NULL},
    };
    assert(cwist_migrate_up(db, extreme, 3) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == INT_MAX);
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
    assert(blob[4] == 0x03);

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

static void test_db_crypt_v3_mac(const cwist_db_crypt_ctx_t *ctx, unsigned char *blob,
                                 size_t blob_len) {
    static const unsigned char info[] = "CWIST db_crypt v3";
    unsigned char salt[CWIST_DB_CRYPT_KEY_LEN] = {0};
    unsigned char prk[CWIST_DB_CRYPT_KEY_LEN];
    unsigned char keys[CWIST_DB_CRYPT_KEY_LEN * 2];
    unsigned char input[CWIST_DB_CRYPT_KEY_LEN + sizeof(info)];
    unsigned int len;
    assert(HMAC(EVP_sha256(), salt, sizeof(salt), ctx->kek, sizeof(ctx->kek), prk, &len));
    for (unsigned int i = 0; i < 2; i++) {
        size_t prefix = i * CWIST_DB_CRYPT_KEY_LEN;
        if (prefix) memcpy(input, keys, prefix);
        memcpy(input + prefix, info, sizeof(info) - 1);
        input[prefix + sizeof(info) - 1] = (unsigned char)(i + 1);
        assert(HMAC(EVP_sha256(), prk, sizeof(prk), input, prefix + sizeof(info),
                    keys + i * CWIST_DB_CRYPT_KEY_LEN, &len));
    }
    assert(HMAC(EVP_sha256(), keys + CWIST_DB_CRYPT_KEY_LEN, CWIST_DB_CRYPT_KEY_LEN, blob,
                blob_len - TEST_DB_CRYPT_AUTH_TAG_LEN,
                blob + blob_len - TEST_DB_CRYPT_AUTH_TAG_LEN, &len));
    assert(len == TEST_DB_CRYPT_AUTH_TAG_LEN);
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
    test_db_crypt_v3_mac(&ctx, blob, blob_len);

    size_t out_len = 0;
    assert(cwist_db_crypt_open(&ctx, blob, blob_len, &out_len) == NULL);
    free(blob);
    printf("  Passed.\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

/* #961: a migration numbered below the newest applied one still runs. */
static void test_migrate_out_of_order(void) {
    printf("Testing migrate up applies a lower missing version...\n");
    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    const cwist_migration_t first[] = {
        {1, "a", "CREATE TABLE a (x);", "DROP TABLE a;"},
        {3, "c", "CREATE TABLE c (x);", "DROP TABLE c;"},
    };
    assert(cwist_migrate_up(db, first, 2) == CWIST_MIGRATE_OK);
    const cwist_migration_t all[] = {
        {1, "a", "CREATE TABLE a (x);", "DROP TABLE a;"},
        {3, "c", "CREATE TABLE c (x);", "DROP TABLE c;"},
        {2, "b", "CREATE TABLE b (x);", "DROP TABLE b;"},
    };
    assert(cwist_migrate_up(db, all, 3) == CWIST_MIGRATE_OK);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE name = 'b';", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM _cwist_migrations;", -1, &stmt, NULL) ==
           SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 3);
    sqlite3_finalize(stmt);
    /* Running again is a no-op. */
    assert(cwist_migrate_up(db, all, 3) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);
    sqlite3_close(db);
    printf("  Passed.\n");
}

int main(void) {
    printf("=== Migration tests ===\n");
    test_migrate_up_and_version();
    test_migrate_idempotent();
    test_migrate_down_one();
    test_migrate_down_all();
    test_migrate_down_steps_skip_irreversible();
    test_migrate_history_failure_rolls_back();
    test_migrate_extreme_versions();
    test_migrate_out_of_order();

    printf("=== DB Crypt tests ===\n");
    test_db_crypt_roundtrip();
    test_db_crypt_wrong_kek();
    test_db_crypt_rejects_tampering();
    test_db_crypt_length_guards();

    printf("\nAll tests passed.\n");
    return 0;
}
