/**
 * @file db_crypt.h
 * @brief Two-level key hierarchy for encrypting an SQLite database blob.
 *
 * Encryption model
 * ================
 * Plaintext SQLite bytes are encrypted with AES-256-CBC using a random
 * Data Encryption Key (DEK).  The DEK is then wrapped (encrypted) with a
 * Key Encryption Key (KEK) that the caller manages.
 *
 * Version 2 wire format produced by cwist_db_crypt_seal()
 * =========================================================
 *  Offset  Length   Content
 *  ------  ------   -------
 *     0      4      Magic "CWDB" (0x43 0x57 0x44 0x42)
 *     4      1      Version byte (0x02)
 *     5     16      KEK IV used to encrypt the DEK blob
 *    21     64      AES-256-CBC encrypted DEK_IV(16) || DEK(32)
 *    85     16      DEK IV used to encrypt the SQLite bytes
 *   101      8      Plaintext length (little-endian uint64)
 *   109      N      AES-256-CBC encrypted SQLite bytes
 * 109+N     32      HMAC-SHA256 over every preceding byte
 *
 * cwist_db_crypt_open() accepts only authenticated v2 blobs. Applications
 * migrating v1 data must explicitly use cwist_db_crypt_open_legacy() and
 * reseal it, which prevents silent downgrade to unauthenticated decryption.
 */

#ifndef __CWIST_DB_CRYPT_H__
#define __CWIST_DB_CRYPT_H__

#include <stddef.h>
#include <stdint.h>

/** @brief Return codes. */
#define CWIST_DB_CRYPT_OK 0
#define CWIST_DB_CRYPT_ERR_ARGS (-1)
#define CWIST_DB_CRYPT_ERR_CRYPTO (-2)
#define CWIST_DB_CRYPT_ERR_FORMAT (-3)
#define CWIST_DB_CRYPT_ERR_MEM (-4)

/** Size constants (bytes). */
#define CWIST_DB_CRYPT_KEY_LEN 32   /**< AES-256 key length. */
#define CWIST_DB_CRYPT_IV_LEN 16   /**< AES block / IV size. */
/** Header overhead: magic(4)+ver(1)+kek_iv(16)+enc_dek(64)+dek_iv(16)+ptlen(8) */
#define CWIST_DB_CRYPT_HDR_LEN 109

/**
 * @brief Caller-managed key pair.
 *
 * Both kek and dek are 32-byte (AES-256) keys.  kek must be provided by the
 * caller.  dek is generated randomly on each cwist_db_crypt_seal() call and
 * stored encrypted in the output blob; it is recovered automatically by
 * cwist_db_crypt_open().
 */
typedef struct cwist_db_crypt_ctx_t {
    unsigned char kek[CWIST_DB_CRYPT_KEY_LEN]; /**< Key Encryption Key. */
} cwist_db_crypt_ctx_t;

/**
 * @brief Encrypt an SQLite database blob.
 *
 * Generates a random DEK and IV pair, encrypts @p sqlite_bytes with the DEK,
 * wraps the DEK under the KEK, and returns the complete sealed blob.
 *
 * @param ctx          Key context (kek must be filled).
 * @param sqlite_bytes Raw SQLite database bytes (from sqlite3_serialize or
 *                     a file read).
 * @param sqlite_len   Length of @p sqlite_bytes.
 * @param[out] out_len Length of the returned blob.
 * @return Heap-allocated sealed blob, or NULL on error.  Caller must free()
 *         the returned pointer.
 */
unsigned char *cwist_db_crypt_seal(const cwist_db_crypt_ctx_t *ctx,
                                   const unsigned char *sqlite_bytes, size_t sqlite_len,
                                   size_t *out_len);

/**
 * @brief Decrypt a sealed blob produced by cwist_db_crypt_seal().
 *
 * Recovers the DEK from the blob header, decrypts the SQLite bytes, and
 * returns the original database content.
 *
 * @param ctx       Key context (kek must match the one used for sealing).
 * @param blob      Sealed blob.
 * @param blob_len  Length of @p blob.
 * @param[out] out_len Length of the returned plaintext.
 * @return Heap-allocated SQLite bytes, or NULL on error.  Caller must free().
 */
unsigned char *cwist_db_crypt_open(const cwist_db_crypt_ctx_t *ctx, const unsigned char *blob,
                                   size_t blob_len, size_t *out_len);

/**
 * @brief Explicitly decrypt an unauthenticated legacy v1 blob for migration.
 *
 * This compatibility path provides no ciphertext integrity. Validate the
 * result and reseal it with cwist_db_crypt_seal() before further use.
 */
unsigned char *cwist_db_crypt_open_legacy(const cwist_db_crypt_ctx_t *ctx,
                                          const unsigned char *blob, size_t blob_len,
                                          size_t *out_len);

#endif /* __CWIST_DB_CRYPT_H__ */
