/**
 * @file cwist_redis.h
 * @brief Minimal in-tree Redis client (RESP2) with connection pooling.
 */

#ifndef __CWIST_REDIS_H__
#define __CWIST_REDIS_H__

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Single Redis connection handle.
 */
typedef struct cwist_redis cwist_redis_t;

/**
 * @brief Redis connection pool handle.
 */
typedef struct cwist_redis_pool cwist_redis_pool_t;

/**
 * @brief Pub/sub message callback.
 */
typedef void (*cwist_redis_msg_cb)(const char *channel, const char *message, void *ctx);

/**
 * @brief Open a TCP connection to a Redis server.
 *
 * @param host Hostname or IP address.
 * @param port TCP port.
 * @return Connection handle, or NULL on failure.
 */
cwist_redis_t *cwist_redis_connect(const char *host, int port);

/**
 * @brief Close a Redis connection.
 */
void cwist_redis_close(cwist_redis_t *r);

/**
 * @brief Execute a raw Redis command and return the bulk reply.
 *
 * @param r        Connection.
 * @param cmd      Space-separated command string (e.g. "GET foo").
 * @param out      If non-NULL and reply is a string, receives a heap copy. Caller frees with
 * cwist_free().
 * @return 0 on success, -1 on error.
 */
cwist_error_t cwist_redis_command(cwist_redis_t *r, const char *cmd, char **out);

/** Execute a binary-safe Redis command expressed as explicit arguments.
 * Reply strings are allocated with cwist_alloc() and may contain NUL bytes;
 * use @p out_len to obtain their exact length. */
cwist_error_t cwist_redis_command_argv(cwist_redis_t *r, size_t argc, const void *const *argv,
                                       const size_t *argv_lens, char **out, size_t *out_len);

/** Authenticate/select a logical Redis database on this connection.
 *
 * @warning AUTH credentials are sent over plaintext TCP (RESP has no
 * encryption by itself). Only use this over loopback, a trusted private
 * network, or behind a TLS-terminating proxy (stunnel/spiped) in front of
 * Redis. */
cwist_error_t cwist_redis_auth(cwist_redis_t *r, const char *username, const char *password);
cwist_error_t cwist_redis_select(cwist_redis_t *r, unsigned int database);

/** Parsed RESP2 reply tree.
 *
 * Supported since v3.8. Returned by cwist_redis_command_argv_reply(). @p type
 * is the RESP type byte: '+', '-', ':', '$', '*'. For '-', @p str holds the
 * error text and the command reports failure. For ':' and '$', @p str is the
 * textual value (always NUL-terminated; use @p len for exact bulk length) and
 * @p integer holds the parsed number. For '*', @p element/@p elements hold
 * children.
 */
typedef struct cwist_redis_reply {
    int type;
    char *str;
    size_t len;
    long long integer;
    struct cwist_redis_reply **element;
    size_t elements;
} cwist_redis_reply_t;

/** Execute a binary-safe command and return the full parsed reply tree.
 *
 * Required for array-reply commands such as XREADGROUP/XAUTOCLAIM. On success
 * *@p reply is set (free with cwist_redis_reply_free()); on a Redis '-' error
 * the tree still describes the error but the returned error code is non-zero. */
cwist_error_t cwist_redis_command_argv_reply(cwist_redis_t *r, size_t argc, const void *const *argv,
                                             const size_t *argv_lens, cwist_redis_reply_t **reply);

/** Release a reply tree returned by cwist_redis_command_argv_reply(). */
void cwist_redis_reply_free(cwist_redis_reply_t *reply);

/**
 * @brief Convenience commands.
 * @{ */
cwist_error_t cwist_redis_get(cwist_redis_t *r, const char *key, char **out_value);
cwist_error_t cwist_redis_set(cwist_redis_t *r, const char *key, const char *value);
cwist_error_t cwist_redis_setex(cwist_redis_t *r, const char *key, const char *value, int seconds);
cwist_error_t cwist_redis_del(cwist_redis_t *r, const char *key);
cwist_error_t cwist_redis_expire(cwist_redis_t *r, const char *key, int seconds);
cwist_error_t cwist_redis_publish(cwist_redis_t *r, const char *channel, const char *message);
/** @} */

/**
 * @brief Subscribe to channels on a dedicated connection.
 *
 * This call blocks the current thread and invokes @p cb for each message.
 * Use it inside a background thread. To stop, call cwist_redis_close() from
 * another thread (the read loop will exit on socket error).
 *
 * @param r         Connection. Must not be shared with other commands.
 * @param channels  NULL-terminated array of channel names.
 * @param cb        Message callback.
 * @param ctx       User context forwarded to callback.
 * @return 0 when stopped cleanly, -1 on error.
 */
cwist_error_t cwist_redis_subscribe(cwist_redis_t *r, const char **channels, cwist_redis_msg_cb cb,
                                    void *ctx);

/**
 * @brief Create a Redis connection pool.
 *
 * @param host      Redis host.
 * @param port      Redis port.
 * @param max_conns Maximum connections in pool.
 * @return Pool handle, or NULL on failure.
 */
cwist_redis_pool_t *cwist_redis_pool_create(const char *host, int port, size_t max_conns);

/**
 * @brief Destroy the pool and close all connections.
 */
void cwist_redis_pool_destroy(cwist_redis_pool_t *pool);

/**
 * @brief Pooled convenience commands.
 * @{ */
cwist_error_t cwist_redis_pool_get(cwist_redis_pool_t *pool, const char *key, char **out_value);
cwist_error_t cwist_redis_pool_set(cwist_redis_pool_t *pool, const char *key, const char *value);
cwist_error_t cwist_redis_pool_setex(cwist_redis_pool_t *pool, const char *key, const char *value,
                                     int seconds);
cwist_error_t cwist_redis_pool_del(cwist_redis_pool_t *pool, const char *key);
cwist_error_t cwist_redis_pool_publish(cwist_redis_pool_t *pool, const char *channel,
                                       const char *message);
cwist_error_t cwist_redis_pool_command_argv(cwist_redis_pool_t *pool, size_t argc,
                                            const void *const *argv, const size_t *argv_lens,
                                            char **out, size_t *out_len);
/** @} */

/**
 * @brief Run one binary-safe pooled command under a single deadline.
 *
 * One CLOCK_MONOTONIC budget of @p timeout_ms covers
 * waiting for a free pool connection, reconnecting a discarded connection,
 * writing the request, and reading the complete reply. The pool's legacy
 * blocking APIs keep their unbounded behavior; pool creation still performs
 * blocking DNS and connects.
 *
 * Connection safety: an idle connection with buffered or readable bytes, a
 * hang-up, or a previous failure is discarded before use and replaced by a
 * new connection to the peer address resolved by cwist_redis_pool_create()
 * (no DNS at request time). Once any request byte has been written the
 * command is never retried; on timeout, transport error, or malformed reply
 * the connection is closed so a late reply can never be read by a later
 * caller. SIGPIPE is not raised. Reply parsing is strict RESP2 with limits
 * (8 nesting levels, 65536 nodes, 8 MiB per bulk string, 16 MiB of payload,
 * 8 KiB status/header lines).
 *
 * Replacement connections start fresh: AUTH, SELECT, HELLO and other
 * per-connection state are not replayed. Pools used with this API must not
 * rely on such state. Legacy pooled calls may also receive a fresh connection
 * after a timed call discards a socket. Use a dedicated default-state pool.
 *
 * Reply types follow cwist_redis_reply_t. A null bulk string is '$' with
 * @c str == NULL and @c integer == -1; a null array is '*' with
 * @c elements == 0 and @c integer == -1. Nested '-' elements are data.
 *
 * The pool must have been created by the calling process (a pool inherited
 * across fork() is rejected without touching it) and must not be destroyed
 * concurrently with this call.
 *
 * @param pool       Pool from cwist_redis_pool_create().
 * @param argc       Number of arguments (1 .. 1048576).
 * @param argv       Argument pointers; an entry may be NULL only if its length is 0.
 * @param argv_lens  Byte length of each argument (frame limited to 512 MiB).
 * @param timeout_ms Total budget in milliseconds; must be positive.
 * @param reply      Out: reply tree on success (free with cwist_redis_reply_free());
 *                   always set to NULL on failure.
 * @return CWIST_ERR_INT16 error whose @c err_i16 is 0 on success or a negative
 *         errno: -EINVAL invalid argument or timeout 0; -EPERM pool inherited
 *         from another process; -ETIMEDOUT budget exhausted; -EIO connect,
 *         send, or receive failure, or peer EOF; -EPROTO malformed or
 *         over-limit reply; -EBADMSG top-level Redis error reply (the
 *         connection stays in sync and reusable); -ENOMEM allocation failure.
 */
cwist_error_t cwist_redis_pool_command_argv_reply_timeout(cwist_redis_pool_t *pool, size_t argc,
                                                          const void *const *argv,
                                                          const size_t *argv_lens,
                                                          unsigned timeout_ms,
                                                          cwist_redis_reply_t **reply);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_REDIS_H__ */
