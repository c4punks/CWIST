/**
 * @file orm_socket.c
 * @brief SQLite-to-socket bridge implementation.
 *
 * This file is the only place in the ORM subsystem that directly
 * links against SQLite3.  It exposes the database as a byte stream
 * over a local Unix socket pair so that cwist_orm_t remains fully
 * decoupled from sqlite3.h.
 *
 * Trust boundary: the worker executes whatever SQL text arrives on its
 * end of the pair, verbatim.  That is safe only because the pair is
 * created with socketpair(AF_UNIX) in cwist_db_transfer_sqlite_to_socket():
 * it has no filesystem path or network address, so no other process can
 * connect to it, and the only peer is the caller's end (normally a
 * cwist_orm_t in the same process).  Anyone holding that caller fd can run
 * arbitrary SQL against the database, so it must never be handed to an
 * untrusted party (SCM_RIGHTS, inheritance across fork() without exec, and
 * so on).  Escaping of untrusted values is the job of the layer that
 * builds the SQL (orm.c), not of this bridge.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <cwist/core/orm/orm_socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdint.h>

#include <sqlite3.h>
#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>

/* ------------------------------------------------------------------------- */
/* Protocol constants                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Magic value placed at the start of every response. */
#define ORM_PROTO_MAGIC 0x43574F52   /* "CWOR" in ASCII hex trickery */

/* ------------------------------------------------------------------------- */
/* Worker context                                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Bundle passed to the background worker thread.
 */
typedef struct {
    int      fd;       /**< Server-side socket descriptor. */
    char    *db_path;  /**< SQLite database path (heap-allocated). */
} cwist_orm_worker_ctx_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Receive exactly @p n bytes from @p fd into @p buf.
 * @return 0 on success, -1 on EOF or error.
 */
static int recv_all(int fd, void *buf, size_t n)
{
    size_t total = 0;
    char *p = (char *)buf;
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, MSG_WAITALL);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

/**
 * @brief Send exactly @p n bytes from @p buf to @p fd.
 * @return 0 on success, -1 on error.
 */
static int send_all(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < n) {
        ssize_t w = send(fd, p + total, n - total, 0);
        if (w <= 0) return -1;
        total += (size_t)w;
    }
    return 0;
}

/**
 * @brief Free a heap-allocated worker context.
 */
static void free_worker_ctx(cwist_orm_worker_ctx_t *ctx)
{
    /* The context is allocated by the caller's thread and freed by the
     * detached worker, so it stays on the raw allocator: a cwist_alloc
     * scope sweep on the caller side must not reclaim it. */
    if (!ctx) return;
    free(ctx->db_path);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* SQLite statement execution                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Add one cell of the current row to the legacy JSON row object.
 *
 * The legacy "rows" payload keeps its text-based typing so existing
 * readers are unaffected: canonical integer text is emitted as raw JSON
 * digits, other numeric-looking text stays a string, and NULL stays null.
 */
static void socket_add_legacy_cell(cJSON *row, sqlite3_stmt *stmt, int i) {
    const char *txt = (const char *)sqlite3_column_text(stmt, i);
    const char *colname = sqlite3_column_name(stmt, i);
    if (!colname) colname = "";

    if (!txt) {
        cJSON_AddNullToObject(row, colname);
        return;
    }

    /* Attempt numeric conversion for cleaner JSON. */
    char *endptr = NULL;
    errno = 0;
    long long ll = strtoll(txt, &endptr, 10);
    char canon[32];
    if (endptr && *endptr == '\0' && errno == 0) {
        /* Emit the digits as raw JSON: cJSON prints numbers with 15
         * significant digits, which corrupted integers from ~1e15 up.
         * Only canonical integer text becomes a number; "007" or
         * " 12" is text that merely parses as one and stays a string. */
        snprintf(canon, sizeof(canon), "%lld", ll);
        if (strcmp(canon, txt) == 0) {
            cJSON_AddRawToObject(row, colname, txt);
        } else {
            cJSON_AddStringToObject(row, colname, txt);
        }
    } else {
        double d = strtod(txt, &endptr);
        if (endptr && *endptr == '\0' && errno == 0) {
            cJSON_AddNumberToObject(row, colname, d);
        } else {
            cJSON_AddStringToObject(row, colname, txt);
        }
    }
}

/** @brief Base64 alphabet for BLOB cells in the typed payload. */
static const char socket_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief Base64-encode @p n bytes of @p data into a new cJSON string.
 * @return The string node, or NULL on allocation failure.
 */
static cJSON *socket_b64_string(const unsigned char *data, int n) {
    size_t out_cap = ((size_t)n / 3 + 1) * 4 + 1;
    char *out = (char *)malloc(out_cap);
    if (!out) return NULL;
    size_t o = 0;
    int i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8) | data[i + 2];
        out[o++] = socket_b64_table[(v >> 18) & 63];
        out[o++] = socket_b64_table[(v >> 12) & 63];
        out[o++] = socket_b64_table[(v >> 6) & 63];
        out[o++] = socket_b64_table[v & 63];
    }
    int rem = n - i;
    if (rem == 1) {
        unsigned v = (unsigned)data[i] << 16;
        out[o++] = socket_b64_table[(v >> 18) & 63];
        out[o++] = socket_b64_table[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8);
        out[o++] = socket_b64_table[(v >> 18) & 63];
        out[o++] = socket_b64_table[(v >> 12) & 63];
        out[o++] = socket_b64_table[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    cJSON *node = cJSON_CreateString(out);
    free(out);
    return node;
}

/**
 * @brief Build the typed cell object for one column of the current row.
 *
 * The cell is typed from the value's SQLite storage class
 * (sqlite3_column_type()), not its text: {"t":"int","v":"-5"} carries
 * the digits as a string so int64 values above 2^53 survive the JSON
 * payload exactly; "real" is a JSON number; "text" a JSON string;
 * "blob" a base64 string with its byte length in "n"; "null" has no
 * payload.  A TEXT value that only looks numeric keeps t=="text".
 */
static cJSON *socket_typed_cell(sqlite3_stmt *stmt, int i, int ctype) {
    cJSON *cell = cJSON_CreateObject();
    if (!cell) return NULL;

    /* Read the typed payload before any text conversion: sqlite3_column_text()
     * changes what sqlite3_column_type() reports for the same column. */
    switch (ctype) {
    case SQLITE_INTEGER: {
        char digits[32];
        snprintf(digits, sizeof(digits), "%lld", (long long)sqlite3_column_int64(stmt, i));
        cJSON_AddStringToObject(cell, "t", "int");
        cJSON_AddStringToObject(cell, "v", digits);
        break;
    }
    case SQLITE_FLOAT:
        cJSON_AddStringToObject(cell, "t", "real");
        cJSON_AddNumberToObject(cell, "v", sqlite3_column_double(stmt, i));
        break;
    case SQLITE_BLOB: {
        const unsigned char *blob = (const unsigned char *)sqlite3_column_blob(stmt, i);
        int n = sqlite3_column_bytes(stmt, i);
        cJSON_AddStringToObject(cell, "t", "blob");
        cJSON_AddItemToObject(cell, "v", socket_b64_string(blob ? blob : (const unsigned char *)"", n));
        cJSON_AddNumberToObject(cell, "n", (double)n);
        break;
    }
    case SQLITE_TEXT:
        cJSON_AddStringToObject(cell, "t", "text");
        cJSON_AddStringToObject(cell, "v", (const char *)sqlite3_column_text(stmt, i));
        break;
    case SQLITE_NULL:
    default:
        cJSON_AddStringToObject(cell, "t", "null");
        break;
    }
    return cell;
}

/**
 * @brief Accumulator used by the statement loop to build both payloads.
 */
typedef struct {
    cJSON *rows;    /**< Legacy cJSON array of row objects (text-typed). */
    cJSON *columns; /**< Typed payload: column names of the row-producing statement. */
    cJSON *trows;   /**< Typed payload: array of row arrays, parallel to @p rows. */
} query_accumulator_t;

/**
 * @brief Run every statement in @p sql, collecting rows into @p acc.
 *
 * Iterates the text statement by statement (sqlite3_prepare_v2's tail
 * pointer), so multi-statement requests such as the MySQL-dialect
 * "INSERT ...; SELECT LAST_INSERT_ID() ..." behave like sqlite3_exec
 * did: rows from all statements merge into one array.  Column names for
 * the typed payload come from the last statement that produces columns.
 *
 * @return SQLITE_OK on success, otherwise the failing statement's code
 *         with @p errmsg set to a heap copy of sqlite3_errmsg().
 */
static int socket_exec_all(sqlite3 *db, const char *sql, query_accumulator_t *acc,
                           char **errmsg) {
    const char *tail = sql;
    int rc = SQLITE_OK;

    while (tail && *tail) {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, tail, -1, &stmt, &tail);
        if (rc != SQLITE_OK) break;
        if (!stmt) continue; /* comment or whitespace between statements */

        int ncols = sqlite3_column_count(stmt);
        if (ncols > 0 && acc->columns) {
            /* Re-shape the typed column list for this statement. */
            cJSON_Delete(acc->columns);
            acc->columns = cJSON_CreateArray();
            if (acc->columns) {
                for (int i = 0; i < ncols; i++) {
                    const char *name = sqlite3_column_name(stmt, i);
                    cJSON_AddItemToArray(acc->columns, cJSON_CreateString(name ? name : ""));
                }
            }
        }

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            cJSON *row = cJSON_CreateObject();
            cJSON *trow = cJSON_CreateArray();
            if (!row || !trow) {
                cJSON_Delete(row);
                cJSON_Delete(trow);
                rc = SQLITE_NOMEM;
                break;
            }
            for (int i = 0; i < ncols; i++) {
                int ctype = sqlite3_column_type(stmt, i);
                cJSON_AddItemToArray(trow, socket_typed_cell(stmt, i, ctype));
                socket_add_legacy_cell(row, stmt, i);
            }
            cJSON_AddItemToArray(acc->rows, row);
            cJSON_AddItemToArray(acc->trows, trow);
        }

        int frc = sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE && frc != SQLITE_OK) rc = frc;
        if (rc != SQLITE_DONE) break;
        rc = SQLITE_OK;
    }

    if (rc != SQLITE_OK && errmsg) *errmsg = strdup(sqlite3_errmsg(db));
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Worker main loop                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Background thread that owns the SQLite connection.
 *
 * Reads framed SQL requests from the socket, executes them, and writes
 * framed JSON responses back.
 */
static void *cwist_orm_socket_worker(void *arg)
{
    cwist_orm_worker_ctx_t *ctx = (cwist_orm_worker_ctx_t *)arg;
    if (!ctx) return NULL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open(ctx->db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ORM-SOCKET] sqlite3_open failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_close(db);
        close(ctx->fd);
        free_worker_ctx(ctx);
        return NULL;
    }

    /* Speed up concurrent access semantics for single-writer scenarios. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    for (;;) {
        /* ---- read request length (4 bytes, big-endian) ---- */
        uint32_t net_len = 0;
        if (recv_all(ctx->fd, &net_len, sizeof(net_len)) != 0) break;
        uint32_t sql_len = ntohl(net_len);
        if (sql_len == 0 || sql_len > 16 * 1024 * 1024) break; /* sanity */

        /* ---- read SQL text ---- */
        char *sql = (char *)cwist_alloc(sql_len + 1);
        if (!sql) break;
        if (recv_all(ctx->fd, sql, sql_len) != 0) {
            cwist_free(sql);
            break;
        }
        sql[sql_len] = '\0';

        /* ---- execute ----
         * sql comes from the in-process peer, not from an external client;
         * see the trust boundary note at the top of this file. */
        query_accumulator_t acc;
        acc.rows = cJSON_CreateArray();
        acc.columns = cJSON_CreateArray();
        acc.trows = cJSON_CreateArray();
        char *errmsg = NULL;

        if (!acc.rows || !acc.columns || !acc.trows) {
            rc = SQLITE_NOMEM;
        } else {
            rc = socket_exec_all(db, sql, &acc, &errmsg);
        }
        cwist_free(sql);

        /* ---- build response payload ---- */
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            cJSON_Delete(acc.rows);
            cJSON_Delete(acc.columns);
            cJSON_Delete(acc.trows);
            if (errmsg) sqlite3_free(errmsg);
            break;
        }

        cJSON_AddNumberToObject(resp, "status", (double)rc);
        if (errmsg) {
            cJSON_AddStringToObject(resp, "error", errmsg);
            free(errmsg);
        } else {
            cJSON_AddNullToObject(resp, "error");
        }
        cJSON_AddItemToObject(resp, "rows", acc.rows);
        cJSON *typed = cJSON_AddObjectToObject(resp, "typed");
        if (typed) {
            cJSON_AddItemToObject(typed, "columns", acc.columns);
            cJSON_AddItemToObject(typed, "rows", acc.trows);
        } else {
            cJSON_Delete(acc.columns);
            cJSON_Delete(acc.trows);
        }

        char *payload = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (!payload) break;

        uint32_t payload_len = (uint32_t)strlen(payload);
        if (payload_len > UINT32_MAX - 1) {
            cJSON_free(payload);
            break;
        }

        /* ---- send framed response ---- */
        uint32_t net_payload = htonl(payload_len);
        if (send_all(ctx->fd, &net_payload, sizeof(net_payload)) != 0) {
            cJSON_free(payload);
            break;
        }
        if (send_all(ctx->fd, payload, payload_len) != 0) {
            cJSON_free(payload);
            break;
        }
        cJSON_free(payload);
    }

    sqlite3_close(db);
    close(ctx->fd);
    free_worker_ctx(ctx);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

static void set_cloexec(int fd) {
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        if (flags != -1) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

int cwist_orm_socketpair_cloexec(int domain, int type, int protocol, int sv[2]) {
    int res = -1;
#if defined(SOCK_CLOEXEC) && SOCK_CLOEXEC != 0
    res = socketpair(domain, type | SOCK_CLOEXEC, protocol, sv);
#endif
    if (res != 0) {
        res = socketpair(domain, type & ~SOCK_CLOEXEC, protocol, sv);
        if (res == 0) {
            set_cloexec(sv[0]);
            set_cloexec(sv[1]);
        }
    } else {
        set_cloexec(sv[0]);
        set_cloexec(sv[1]);
    }
    return res;
}


int cwist_db_transfer_sqlite_to_socket(const char *db_path)
{
    if (!db_path) {
        errno = EINVAL;
        return -1;
    }

    int sv[2];
    /* SOCK_CLOEXEC is not available on every BSD-family system (macOS lacks
     * it); the helper falls back to fcntl(FD_CLOEXEC) there. */
    if (cwist_orm_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return -1;
    }

    cwist_orm_worker_ctx_t *ctx =
        (cwist_orm_worker_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    ctx->fd = sv[1];
    ctx->db_path = strdup(db_path);
    if (!ctx->db_path) {
        free(ctx);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int rc = pthread_create(&tid, &attr, cwist_orm_socket_worker, ctx);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free_worker_ctx(ctx);
        close(sv[0]);
        close(sv[1]);
        errno = rc;
        return -1;
    }

    return sv[0]; /* caller owns this end */
}
