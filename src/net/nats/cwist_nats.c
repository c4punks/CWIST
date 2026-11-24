/** @file cwist_nats.c
 * @brief cwist_nats.c interface.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/net/nats/cwist_nats.h>
#include <cwist/core/mem/alloc.h>
#include <nats.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cwist_nats {
    natsConnection *conn;
    natsSubscription *sub;
    cwist_nats_msg_cb user_cb;
    void *user_ctx;
    /* Set by cnats' completion callback once the subscription is closed and
     * its message handler has returned for the last time (#981). */
    pthread_mutex_t done_mu;
    pthread_cond_t done_cv;
    bool sub_done;
};

/** Upper bound on how long destroy waits for an in-flight message callback. */
#define CWIST_NATS_DESTROY_WAIT_MS 5000

/** @brief cnats completion callback: the handler will not run again. */
static void cwist_nats_on_complete(void *closure) {
    cwist_nats_t *nats = (cwist_nats_t *)closure;
    pthread_mutex_lock(&nats->done_mu);
    nats->sub_done = true;
    pthread_cond_broadcast(&nats->done_cv);
    pthread_mutex_unlock(&nats->done_mu);
}

/** @brief NATS callback shim that forwards a message to the user callback.
 *
 * Extracts the subject and payload from the NATS message and invokes the
 * user-registered callback with the user context stored in the handle.
 * Always destroys the message before returning.
 *
 * @param nc NATS connection (unused).
 * @param sub NATS subscription (unused).
 * @param msg Message to deliver; ownership is taken and it is always destroyed.
 * @param closure cwist_nats_t handle carrying the user callback and context.
 */
static void cwist_nats_adapter(natsConnection *nc, natsSubscription *sub, natsMsg *msg,
                               void *closure) {
    (void)nc;
    (void)sub;
    cwist_nats_t *nats = (cwist_nats_t *)closure;
    if (nats && nats->user_cb) {
        const char *subject = natsMsg_GetSubject(msg);
        const char *data = natsMsg_GetData(msg);
        int len = natsMsg_GetDataLength(msg);
        nats->user_cb(subject, data, (size_t)len, nats->user_ctx);
    }
    natsMsg_Destroy(msg);
}

/** @brief Connect to a NATS server and create a handle.
 *
 * Allocates a cwist_nats_t and connects to the given URL, or to the NATS
 * default URL when @p url is NULL.
 *
 * @param[out] nats Receives the new handle on success; must not be NULL.
 * @param url Server URL, or NULL for the default server.
 * @return cwist_error_t with err_i16 set to 0 on success, CWIST_ERROR_INVALID_PARAM
 *         if @p nats is NULL, CWIST_ERROR_NOMEM on allocation failure, or the
 *         natsStatus code on connection failure.
 */
cwist_error_t cwist_nats_connect(cwist_nats_t **nats, const char *url) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    *nats = NULL;
    cwist_nats_t *obj = (cwist_nats_t *)cwist_alloc(sizeof(cwist_nats_t));
    if (!obj) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    memset(obj, 0, sizeof(*obj));
    if (pthread_mutex_init(&obj->done_mu, NULL) != 0) {
        cwist_free(obj);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    if (pthread_cond_init(&obj->done_cv, NULL) != 0) {
        pthread_mutex_destroy(&obj->done_mu);
        cwist_free(obj);
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    natsStatus s = natsConnection_ConnectTo(&obj->conn, url ? url : NATS_DEFAULT_URL);
    if (s != NATS_OK) {
        pthread_cond_destroy(&obj->done_cv);
        pthread_mutex_destroy(&obj->done_mu);
        cwist_free(obj);
        err.error.err_i16 = (int16_t)s;
        return err;
    }
    *nats = obj;
    err.error.err_i16 = 0;
    return err;
}

/** @brief Subscribe to a NATS subject and register a message callback.
 *
 * Each handle supports one subscription. A second subscribe attempt is
 * rejected without changing the existing subscription or callback pair. The
 * callback is invoked from the NATS delivery thread via cwist_nats_adapter.
 *
 * @param nats Handle with an active connection; must not be NULL.
 * @param subject Subject to subscribe to; must not be NULL.
 * @param cb Callback invoked for each incoming message; may be NULL.
 * @param ctx Opaque pointer passed back to @p cb.
 * @return cwist_error_t with err_i16 set to 0 on success, CWIST_ERROR_INVALID_PARAM
 *         on NULL arguments or an existing subscription, or the natsStatus code on failure.
 */
cwist_error_t cwist_nats_subscribe(cwist_nats_t *nats, const char *subject, cwist_nats_msg_cb cb,
                                   void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats || !nats->conn || !subject || nats->sub) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    nats->user_cb = cb;
    nats->user_ctx = ctx;
    nats->sub_done = false;
    natsStatus s =
        natsConnection_Subscribe(&nats->sub, nats->conn, subject, cwist_nats_adapter, nats);
    if (s == NATS_OK) s = natsSubscription_SetOnCompleteCB(nats->sub, cwist_nats_on_complete, nats);
    if (s != NATS_OK) {
        if (nats->sub) {
            natsSubscription_Unsubscribe(nats->sub);
            natsSubscription_Destroy(nats->sub);
            nats->sub = NULL;
        }
        nats->user_cb = NULL;
        nats->user_ctx = NULL;
        err.error.err_i16 = (int16_t)s;
        return err;
    }
    err.error.err_i16 = 0;
    return err;
}

/** @brief Publish a NUL-terminated string message to a subject.
 *
 * A NULL @p data is published as an empty string. Flushes the connection
 * after publishing so the message is written out before returning.
 *
 * @param nats Handle with an active connection; must not be NULL.
 * @param subject Subject to publish to; must not be NULL.
 * @param data Payload string, or NULL to send an empty payload.
 * @return cwist_error_t with err_i16 set to 0 on success, CWIST_ERROR_INVALID_PARAM
 *         on NULL arguments, or the natsStatus code on publish/flush failure.
 */
cwist_error_t cwist_nats_publish_string(cwist_nats_t *nats, const char *subject, const char *data) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats || !nats->conn || !subject) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    natsStatus s = natsConnection_PublishString(nats->conn, subject, data ? data : "");
    if (s == NATS_OK) {
        s = natsConnection_Flush(nats->conn);
    }
    err.error.err_i16 = (int16_t)s;
    return err;
}

/** @brief Pump pending I/O on the connection.
 *
 * Flushes the connection, pushing buffered outgoing messages and processing
 * incoming traffic so delivered callbacks can run. Safe to call with a NULL
 * or disconnected handle; such calls are no-ops.
 *
 * @param nats Handle to flush, or NULL.
 */
void cwist_nats_dispatch(cwist_nats_t *nats) {
    if (nats && nats->conn) {
        natsConnection_Flush(nats->conn);
    }
}

/** @brief Destroy a handle and release its resources.
 *
 * Unsubscribes and disconnects if still active, then frees the handle.
 * Waits (up to CWIST_NATS_DESTROY_WAIT_MS) until the message callback has
 * returned for the last time, so it never runs on a freed handle. Must not
 * be called from inside the message callback. Safe to call with NULL; the
 * handle must not be used afterwards.
 *
 * @param nats Handle to destroy, or NULL.
 */
void cwist_nats_destroy(cwist_nats_t *nats) {
    if (!nats) return;
    if (nats->sub) {
        /* The message handler may still be running, or run once more, on the
         * delivery thread after the unsubscribe (see
         * natsSubscription_SetOnCompleteCB in nats.h). Wait for the completion
         * callback before freeing anything the handler reads (#981). */
        (void)natsSubscription_Unsubscribe(nats->sub);
        bool done;
        {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += CWIST_NATS_DESTROY_WAIT_MS / 1000;
            deadline.tv_nsec += (long)(CWIST_NATS_DESTROY_WAIT_MS % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&nats->done_mu);
            while (!nats->sub_done) {
                if (pthread_cond_timedwait(&nats->done_cv, &nats->done_mu, &deadline) ==
                    ETIMEDOUT)
                    break;
            }
            done = nats->sub_done;
            pthread_mutex_unlock(&nats->done_mu);
        }
        natsSubscription_Destroy(nats->sub);
        nats->sub = NULL;
        if (!done) {
            /* A handler is still running past the bound: leak the handle
             * rather than free memory it is using. */
            if (nats->conn) natsConnection_Destroy(nats->conn);
            nats->conn = NULL;
            return;
        }
    }
    if (nats->conn) {
        natsConnection_Destroy(nats->conn);
    }
    pthread_cond_destroy(&nats->done_cv);
    pthread_mutex_destroy(&nats->done_mu);
    cwist_free(nats);
}

/** @brief Access the underlying NATS connection.
 *
 * The returned pointer is owned by the handle and remains valid until
 * cwist_nats_destroy is called.
 *
 * @param nats Handle to query, or NULL.
 * @return The native natsConnection, or NULL if @p nats is NULL.
 */
natsConnection *cwist_nats_native(cwist_nats_t *nats) {
    return nats ? nats->conn : NULL;
}
