/**
 * @file db_sync.h
 * @brief Server-to-server SQLite database synchronisation over TCP.
 *
 * The server serialises its in-memory SQLite database, encrypts the bytes
 * using the KEK/DEK hierarchy from db_crypt, and streams the sealed blob to
 * any connecting client.
 *
 * Wire protocol (per-connection)
 * ================================
 *  Client → Server: magic, fresh client nonce, keyed client authenticator
 *  Server → Client: fresh server nonce, keyed server authenticator
 *  Server → Client: 8-byte little-endian uint64 blob_length and sealed blob
 *  Server → Client: keyed authenticator binding the blob to both nonces
 *
 * Unauthenticated clients are rejected without consuming the one-shot send.
 * The client verifies both authenticators before opening the blob with
 * cwist_db_crypt_open(), preventing recorded sessions from being replayed.
 */

#ifndef __CWIST_DB_SYNC_H__
#define __CWIST_DB_SYNC_H__

#include <cwist/security/db_crypt/db_crypt.h>
#include <sqlite3.h>
#include <stddef.h>

#define CWIST_DB_SYNC_DEFAULT_PORT 9877

/** @brief Return codes. */
#define CWIST_DB_SYNC_OK 0
#define CWIST_DB_SYNC_ERR_ARGS (-1)
#define CWIST_DB_SYNC_ERR_NET (-2)
#define CWIST_DB_SYNC_ERR_CRYPTO (-3)
#define CWIST_DB_SYNC_ERR_MEM (-4)
#define CWIST_DB_SYNC_ERR_PROTO (-5)

/**
 * @brief Serve the database once: accept one connection, send the sealed blob,
 *        then close.
 *
 * Serialises @p db using sqlite3_serialize(), seals the bytes with @p ctx,
 * listens on @p port, accepts the first authorised connection, sends the
 * sealed blob, and returns.  This is intentionally single-shot so the caller
 * can embed it in a loop or background thread.
 *
 * @param db   Source SQLite3 handle (read-locked during serialise).
 * @param ctx  Encryption key context.
 * @param port TCP port to listen on.
 * @return CWIST_DB_SYNC_OK on success, negative on failure.
 */
int cwist_db_sync_serve(sqlite3 *db, const cwist_db_crypt_ctx_t *ctx, int port);

/**
 * @brief Pull a database from a sync server.
 *
 * Connects to host:port, exchanges the handshake, receives the sealed blob,
 * decrypts it, and returns the raw SQLite bytes.  The caller is responsible
 * for loading the bytes into an SQLite instance via sqlite3_deserialize().
 *
 * @param host     Hostname or IP of the sync server.
 * @param port     TCP port of the sync server.
 * @param ctx      Decryption key context (kek must match server's kek).
 * @param[out] out_bytes Decrypted SQLite bytes (caller must free()).
 * @param[out] out_len   Length of @p out_bytes.
 * @return CWIST_DB_SYNC_OK on success, negative on failure.
 */
int cwist_db_sync_pull(const char *host, int port, const cwist_db_crypt_ctx_t *ctx,
                       unsigned char **out_bytes, size_t *out_len);

#endif /* __CWIST_DB_SYNC_H__ */
