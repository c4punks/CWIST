/**
 * @file graphql_ws.c
 * @brief Experimental GraphQL subscriptions over the graphql-ws WebSocket
 * subprotocol (v3.7 Phase 4).
 *
 * Protocol (Sec-WebSocket-Protocol: graphql-transport-ws):
 *   connection_init  -> connection_ack
 *   subscribe {id, payload:{query,variables,operationName}}
 *                    -> one `next` {id, payload:{data:{field: <event>}}} per
 *                       published event; `error` {id, payload:[...]} when the
 *                       operation fails to start; `complete` {id} when the
 *                       operation ends (client `complete` or socket close)
 *   ping/pong        -> JSON keepalive at any time after init
 *   subscribe before connection_init closes the socket with 4429; other
 *   protocol violations close with 4400/4401/4409.
 *
 * Broker model: a subscription resolver returns an event source (a set of
 * topics). cwist_graphql_publish() is the single fan-out entry point and may
 * be called from ANY thread: matched operations queue a serialized `next`
 * message, and a reactor-posted flush node drains the queue on the owning
 * reactor thread (only the reactor thread may touch the socket). Connection
 * teardown is reported by the WS layer's on_close hook, which unlinks every
 * operation of the connection from the broker and releases the protocol
 * state; refcounting keeps the state alive until in-flight flush nodes have
 * drained.
 */
#include <cwist/net/graphql/graphql_ws.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/websocket/websocket_async.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../websocket/ws_async_internal.h"

/* graphql-ws close codes (https://github.com/enisdenjo/graphql-ws/blob/master/PROTOCOL.md). */
#define GQL_WS_CLOSE_BAD_REQUEST 4400
#define GQL_WS_CLOSE_UNAUTHORIZED 4401
#define GQL_WS_CLOSE_DUPLICATE_ID 4409
#define GQL_WS_CLOSE_SUBSCRIBE_BEFORE_INIT 4429

/* ------------------------------------------------------------------ */
/* Event source                                                        */
/* ------------------------------------------------------------------ */

struct cwist_graphql_event_source {
    char **topics;
    size_t topic_count;
    size_t topic_cap;
};

cwist_graphql_event_source_t *cwist_graphql_event_source_create(void) {
    return (cwist_graphql_event_source_t *)cwist_alloc(sizeof(cwist_graphql_event_source_t));
}

bool cwist_graphql_event_source_add_topic(cwist_graphql_event_source_t *src, const char *topic) {
    if (!src || !topic || topic[0] == '\0') return false;
    for (size_t i = 0; i < src->topic_count; i++) {
        if (strcmp(src->topics[i], topic) == 0) return true; /* idempotent */
    }
    if (src->topic_count == src->topic_cap) {
        size_t new_cap = src->topic_cap ? src->topic_cap * 2 : 4;
        char **nt = (char **)cwist_alloc_array(new_cap, sizeof(char *));
        if (!nt) return false;
        if (src->topic_count > 0) memcpy(nt, src->topics, src->topic_count * sizeof(char *));
        cwist_free(src->topics);
        src->topics = nt;
        src->topic_cap = new_cap;
    }
    char *copy = cwist_strdup(topic);
    if (!copy) return false;
    src->topics[src->topic_count++] = copy;
    return true;
}

void cwist_graphql_event_source_destroy(cwist_graphql_event_source_t *src) {
    if (!src) return;
    for (size_t i = 0; i < src->topic_count; i++) cwist_free(src->topics[i]);
    cwist_free(src->topics);
    cwist_free(src);
}

static bool gql_src_has_topic(const cwist_graphql_event_source_t *src, const char *topic) {
    for (size_t i = 0; i < src->topic_count; i++) {
        if (strcmp(src->topics[i], topic) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Endpoint (subscription field registry)                              */
/* ------------------------------------------------------------------ */

typedef struct gql_sub_field {
    char *name;
    cwist_graphql_subscribe_fn fn;
    void *ctx;
    struct gql_sub_field *next;
} gql_sub_field_t;

struct cwist_graphql_ws {
    cwist_graphql_schema_t *schema; /* not owned */
    gql_sub_field_t *subs;
};

cwist_graphql_ws_t *cwist_graphql_ws_create(cwist_graphql_schema_t *schema) {
    if (!schema) return NULL;
    cwist_graphql_ws_t *gws = (cwist_graphql_ws_t *)cwist_alloc(sizeof(*gws));
    if (gws) {
        gws->schema = schema;
        gws->subs = NULL;
    }
    return gws;
}

void cwist_graphql_ws_destroy(cwist_graphql_ws_t *gws) {
    if (!gws) return;
    gql_sub_field_t *f = gws->subs;
    while (f) {
        gql_sub_field_t *next = f->next;
        cwist_free(f->name);
        cwist_free(f);
        f = next;
    }
    cwist_free(gws);
}

bool cwist_graphql_ws_add_subscription(cwist_graphql_ws_t *gws, const char *field,
                                       cwist_graphql_subscribe_fn subscribe_fn, void *ctx) {
    if (!gws || !field || field[0] == '\0' || !subscribe_fn) return false;
    for (gql_sub_field_t *f = gws->subs; f; f = f->next) {
        if (strcmp(f->name, field) == 0) {
            f->fn = subscribe_fn;
            f->ctx = ctx;
            return true;
        }
    }
    gql_sub_field_t *f = (gql_sub_field_t *)cwist_alloc(sizeof(*f));
    if (!f) return false;
    f->name = cwist_strdup(field);
    if (!f->name) {
        cwist_free(f);
        return false;
    }
    f->fn = subscribe_fn;
    f->ctx = ctx;
    f->next = gws->subs;
    gws->subs = f;
    return true;
}

/* ------------------------------------------------------------------ */
/* Broker: operations, connections, pending queues                     */
/* ------------------------------------------------------------------ */

/* One live `subscribe` operation. Operations live in the global broker list
 * while active; each belongs to exactly one connection. */
typedef struct gql_ws_op {
    char *id;     /* client operation id (owned) */
    char *field;  /* subscription root field name (owned) */
    cwist_graphql_event_source_t *src; /* topic interest (owned) */
    struct gql_ws_conn *conn;
    struct gql_ws_op *next;
} gql_ws_op_t;

typedef struct gql_ws_conn {
    cwist_graphql_ws_t *gws;   /* not owned */
    cwist_websocket_async *ws; /* NULL until attach succeeds; cleared at teardown */
    cwist_reactor_t *reactor;
    atomic_int refs;
    bool init_done; /* connection_init received (reactor thread only) */
    bool dead;      /* teardown started; guarded by g_broker_lock */
    /* Queued `next` messages (malloc'd strings via cJSON_Print). Guarded by
     * g_broker_lock; drained by flush nodes on the reactor thread. */
    char **pending;
    size_t pending_count;
    size_t pending_cap;
} gql_ws_conn_t;

/* Reactor-posted flush node: drains a connection's pending queue on the
 * reactor thread. Holds a conn reference until dispatched. */
typedef struct gql_ws_flush {
    cwist_reactor_post_t node;
    gql_ws_conn_t *conn;
} gql_ws_flush_t;

static pthread_mutex_t g_broker_lock = PTHREAD_MUTEX_INITIALIZER;
static gql_ws_op_t *g_broker_ops; /* all active operations, all endpoints */

static void gql_ws_conn_ref(gql_ws_conn_t *conn) {
    atomic_fetch_add_explicit(&conn->refs, 1, memory_order_relaxed);
}

/* Frees the connection state once the WS layer, the protocol layer, and all
 * in-flight flush nodes have released their references. */
static void gql_ws_conn_free(gql_ws_conn_t *conn) {
    for (size_t i = 0; i < conn->pending_count; i++) cwist_free(conn->pending[i]);
    cwist_free(conn->pending);
    cwist_free(conn);
}

static void gql_ws_conn_unref(gql_ws_conn_t *conn) {
    if (atomic_fetch_sub_explicit(&conn->refs, 1, memory_order_acq_rel) == 1)
        gql_ws_conn_free(conn);
}

static void gql_ws_op_free(gql_ws_op_t *op) {
    cwist_free(op->id);
    cwist_free(op->field);
    cwist_graphql_event_source_destroy(op->src);
    cwist_free(op);
}

/* Remove @p victim from the global broker list (caller holds g_broker_lock). */
static void gql_ws_op_unlink(gql_ws_op_t *victim) {
    gql_ws_op_t **pp = &g_broker_ops;
    while (*pp && *pp != victim) pp = &(*pp)->next;
    if (*pp) *pp = victim->next;
}

/* Unlink and free every operation of @p conn (caller holds g_broker_lock). */
static void gql_ws_conn_purge_ops(gql_ws_conn_t *conn) {
    gql_ws_op_t *op = g_broker_ops;
    while (op) {
        gql_ws_op_t *next = op->next;
        if (op->conn == conn) {
            gql_ws_op_unlink(op);
            gql_ws_op_free(op);
        }
        op = next;
    }
}

/* ------------------------------------------------------------------ */
/* Wire helpers (reactor thread only)                                  */
/* ------------------------------------------------------------------ */

static void gql_ws_send_json(cwist_websocket_async *ws, cJSON *msg) {
    char *printed = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!printed) {
        cwist_websocket_async_close(ws);
        return;
    }
    if (cwist_websocket_async_send(ws, CWIST_WS_FRAME_TEXT, (const uint8_t *)printed,
                                   strlen(printed)) != 0)
        cwist_websocket_async_close(ws);
    cwist_free(printed);
}

static void gql_ws_send_error(cwist_websocket_async *ws, const char *id, const char *message) {
    cJSON *err = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateArray();
    cJSON *entry = cJSON_CreateObject();
    if (!err || !payload || !entry) {
        cJSON_Delete(err);
        cJSON_Delete(payload);
        cJSON_Delete(entry);
        cwist_websocket_async_close(ws);
        return;
    }
    cJSON_AddStringToObject(err, "type", "error");
    cJSON_AddStringToObject(err, "id", id ? id : "");
    cJSON_AddStringToObject(entry, "message", message);
    cJSON_AddItemToArray(payload, entry);
    cJSON_AddItemToObject(err, "payload", payload);
    gql_ws_send_json(ws, err);
}

/* ------------------------------------------------------------------ */
/* Pending queue + flush delivery                                      */
/* ------------------------------------------------------------------ */

/* Append a `next` message to the connection's queue (caller holds g_broker_lock). */
static bool gql_ws_pending_push(gql_ws_conn_t *conn, char *msg) {
    if (conn->pending_count == conn->pending_cap) {
        size_t new_cap = conn->pending_cap ? conn->pending_cap * 2 : 8;
        char **np = (char **)cwist_alloc_array(new_cap, sizeof(char *));
        if (!np) return false;
        if (conn->pending_count > 0)
            memcpy(np, conn->pending, conn->pending_count * sizeof(char *));
        cwist_free(conn->pending);
        conn->pending = np;
        conn->pending_cap = new_cap;
    }
    conn->pending[conn->pending_count++] = msg;
    return true;
}

/* Serialize one `next` message for @p op (malloc'd string, or NULL on OOM). */
static char *gql_ws_build_next(const gql_ws_op_t *op, const cJSON *payload) {
    char *printed = NULL;
    cJSON *msg = cJSON_CreateObject();
    cJSON *pl = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *value = payload ? cJSON_Duplicate(payload, true) : cJSON_CreateNull();
    if (!msg || !pl || !data || !value) goto out;
    cJSON_AddStringToObject(msg, "type", "next");
    cJSON_AddStringToObject(msg, "id", op->id);
    cJSON_AddItemToObject(data, op->field, value);
    value = NULL;
    cJSON_AddItemToObject(pl, "data", data);
    data = NULL;
    cJSON_AddItemToObject(msg, "payload", pl);
    pl = NULL;
    printed = cJSON_PrintUnformatted(msg);
out:
    cJSON_Delete(value);
    cJSON_Delete(data);
    cJSON_Delete(pl);
    cJSON_Delete(msg);
    return printed;
}

/* Runs on the reactor thread; drains the pending queue to the wire. */
static void gql_ws_flush_cb(void *ctx) {
    gql_ws_flush_t *flush = (gql_ws_flush_t *)ctx;
    gql_ws_conn_t *conn = flush->conn;
    char **msgs = NULL;
    size_t count = 0;

    pthread_mutex_lock(&g_broker_lock);
    /* Teardown runs on this same thread, so conn->ws is stable here: once
     * dead, the socket is gone and the queue was already freed. A NULL ws
     * (attach still finalizing) leaves the queue for a later flush. */
    if (!conn->dead && conn->ws && conn->pending_count > 0) {
        count = conn->pending_count;
        msgs = (char **)cwist_alloc_array(count, sizeof(char *));
        if (msgs) {
            memcpy(msgs, conn->pending, count * sizeof(char *));
            conn->pending_count = 0;
        } else {
            count = 0; /* OOM: leave queued for the next flush */
        }
    }
    pthread_mutex_unlock(&g_broker_lock);

    for (size_t i = 0; i < count; i++) {
        if (cwist_websocket_async_send(conn->ws, CWIST_WS_FRAME_TEXT, (const uint8_t *)msgs[i],
                                       strlen(msgs[i])) != 0) {
            cwist_websocket_async_close(conn->ws);
        }
        cwist_free(msgs[i]);
    }
    cwist_free(msgs);

    gql_ws_conn_unref(conn);
    cwist_free(flush);
}

/* Queue a flush node for @p conn; takes a reference released by the callback. */
static void gql_ws_post_flush(gql_ws_conn_t *conn) {
    gql_ws_flush_t *flush = (gql_ws_flush_t *)cwist_alloc(sizeof(*flush));
    if (!flush) return;
    flush->conn = conn;
    flush->node.cb = gql_ws_flush_cb;
    flush->node.ctx = flush;
    gql_ws_conn_ref(conn);
    if (!cwist_reactor_post(conn->reactor, &flush->node)) {
        gql_ws_conn_unref(conn);
        cwist_free(flush);
    }
}

size_t cwist_graphql_publish(const char *topic, const cJSON *payload) {
    if (!topic || topic[0] == '\0') return 0;

    gql_ws_conn_t **conns = NULL;
    size_t conn_count = 0, conn_cap = 0;
    size_t delivered = 0;

    pthread_mutex_lock(&g_broker_lock);
    for (gql_ws_op_t *op = g_broker_ops; op; op = op->next) {
        if (!gql_src_has_topic(op->src, topic)) continue;
        char *msg = gql_ws_build_next(op, payload);
        if (!msg) continue;
        if (!gql_ws_pending_push(op->conn, msg)) {
            cwist_free(msg);
            continue;
        }
        delivered++;
        bool seen = false;
        for (size_t i = 0; i < conn_count; i++) {
            if (conns[i] == op->conn) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (conn_count == conn_cap) {
                size_t new_cap = conn_cap ? conn_cap * 2 : 4;
                gql_ws_conn_t **nc = (gql_ws_conn_t **)cwist_alloc_array(new_cap, sizeof(*nc));
                if (!nc) continue; /* queue stays; drained by an earlier flush */
                if (conn_count > 0) memcpy(nc, conns, conn_count * sizeof(*nc));
                cwist_free(conns);
                conns = nc;
                conn_cap = new_cap;
            }
            conns[conn_count] = op->conn;
            gql_ws_conn_ref(op->conn);
            conn_count++;
        }
    }
    pthread_mutex_unlock(&g_broker_lock);

    for (size_t i = 0; i < conn_count; i++) {
        gql_ws_post_flush(conns[i]); /* adopts the reference */
        gql_ws_conn_unref(conns[i]); /* release the publish-side reference */
    }
    cwist_free(conns);
    return delivered;
}

/* ------------------------------------------------------------------ */
/* Protocol handling (reactor thread only)                             */
/* ------------------------------------------------------------------ */

static void gql_ws_handle_subscribe(cwist_websocket_async *ws, gql_ws_conn_t *conn,
                                    const cJSON *msg) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsString(id) || !id->valuestring || id->valuestring[0] == '\0') {
        gql_ws_send_error(ws, NULL, "Subscribe message must carry a non-empty string 'id'");
        return;
    }
    const char *op_id = id->valuestring;

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(msg, "payload");
    const cJSON *query =
        cJSON_IsObject(payload) ? cJSON_GetObjectItemCaseSensitive(payload, "query") : NULL;
    if (!cJSON_IsString(query) || !query->valuestring ||
        strlen(query->valuestring) > CWIST_GRAPHQL_MAX_QUERY_SIZE) {
        gql_ws_send_error(ws, op_id, "Subscribe payload must contain a string 'query'");
        return;
    }

    pthread_mutex_lock(&g_broker_lock);
    bool duplicate = false;
    for (gql_ws_op_t *op = g_broker_ops; op; op = op->next) {
        if (op->conn == conn && strcmp(op->id, op_id) == 0) {
            duplicate = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_broker_lock);
    if (duplicate) {
        /* graphql-ws: a duplicate subscriber id is a connection-level
         * violation, not an operation-level one. */
        cwist_websocket_async_close_code(ws, GQL_WS_CLOSE_DUPLICATE_ID);
        return;
    }

    const cJSON *variables =
        cJSON_IsObject(payload) ? cJSON_GetObjectItemCaseSensitive(payload, "variables") : NULL;

    char field[128];
    cJSON *args = NULL;
    if (!cwist_graphql_parse_subscription(query->valuestring, variables, field, sizeof(field),
                                          &args)) {
        gql_ws_send_error(ws, op_id, "Expected a 'subscription' operation with one root field");
        return;
    }

    gql_sub_field_t *sf = NULL;
    for (gql_sub_field_t *f = conn->gws->subs; f; f = f->next) {
        if (strcmp(f->name, field) == 0) {
            sf = f;
            break;
        }
    }
    if (!sf) {
        cJSON_Delete(args);
        char buf[192];
        snprintf(buf, sizeof(buf), "Cannot subscribe field '%s' on schema", field);
        gql_ws_send_error(ws, op_id, buf);
        return;
    }

    cwist_graphql_event_source_t *src = sf->fn(args, variables, sf->ctx);
    cJSON_Delete(args);
    if (!src) {
        gql_ws_send_error(ws, op_id, "Subscription resolver rejected the operation");
        return;
    }

    gql_ws_op_t *op = (gql_ws_op_t *)cwist_alloc(sizeof(*op));
    if (!op) {
        cwist_graphql_event_source_destroy(src);
        gql_ws_send_error(ws, op_id, "Out of memory");
        return;
    }
    op->id = cwist_strdup(op_id);
    op->field = cwist_strdup(field);
    if (!op->id || !op->field) {
        cwist_free(op->id);
        cwist_free(op->field);
        cwist_free(op);
        cwist_graphql_event_source_destroy(src);
        gql_ws_send_error(ws, op_id, "Out of memory");
        return;
    }
    op->src = src;
    op->conn = conn;

    pthread_mutex_lock(&g_broker_lock);
    op->next = g_broker_ops;
    g_broker_ops = op;
    pthread_mutex_unlock(&g_broker_lock);
}

static void gql_ws_dispatch(cwist_websocket_async *ws, gql_ws_conn_t *conn, cJSON *msg) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        gql_ws_send_error(ws, NULL, "Message must carry a string 'type'");
        return;
    }
    const char *t = type->valuestring;

    if (!conn->init_done) {
        if (strcmp(t, "connection_init") == 0) {
            conn->init_done = true;
            cJSON *ack = cJSON_CreateObject();
            if (!ack) {
                cwist_websocket_async_close(ws);
                return;
            }
            cJSON_AddStringToObject(ack, "type", "connection_ack");
            gql_ws_send_json(ws, ack);
        } else if (strcmp(t, "subscribe") == 0) {
            cwist_websocket_async_close_code(ws, GQL_WS_CLOSE_SUBSCRIBE_BEFORE_INIT);
        } else {
            cwist_websocket_async_close_code(ws, GQL_WS_CLOSE_UNAUTHORIZED);
        }
        return;
    }
    if (strcmp(t, "ping") == 0) {
        cJSON *pong = cJSON_CreateObject();
        if (!pong) {
            cwist_websocket_async_close(ws);
            return;
        }
        cJSON_AddStringToObject(pong, "type", "pong");
        const cJSON *pl = cJSON_GetObjectItemCaseSensitive(msg, "payload");
        if (pl) cJSON_AddItemToObject(pong, "payload", cJSON_Duplicate(pl, true));
        gql_ws_send_json(ws, pong);
    } else if (strcmp(t, "pong") == 0 || strcmp(t, "connection_init") == 0) {
        /* Keepalive / duplicate init: nothing to do. */
    } else if (strcmp(t, "subscribe") == 0) {
        gql_ws_handle_subscribe(ws, conn, msg);
    } else if (strcmp(t, "complete") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
        if (!cJSON_IsString(id) || !id->valuestring) {
            gql_ws_send_error(ws, NULL, "Complete message must carry a string 'id'");
            return;
        }
        pthread_mutex_lock(&g_broker_lock);
        for (gql_ws_op_t *op = g_broker_ops; op; op = op->next) {
            if (op->conn == conn && strcmp(op->id, id->valuestring) == 0) {
                gql_ws_op_unlink(op);
                gql_ws_op_free(op);
                break;
            }
        }
        pthread_mutex_unlock(&g_broker_lock);
        /* A client `complete` gets no response; the operation is simply gone. */
    } else {
        cwist_websocket_async_close_code(ws, GQL_WS_CLOSE_BAD_REQUEST);
    }
}

/* ------------------------------------------------------------------ */
/* Connection lifecycle                                                */
/* ------------------------------------------------------------------ */

static gql_ws_conn_t *gql_ws_conn_create(cwist_graphql_ws_t *gws, cwist_reactor_t *reactor) {
    gql_ws_conn_t *conn = (gql_ws_conn_t *)cwist_alloc(sizeof(*conn));
    if (!conn) return NULL;
    conn->gws = gws;
    conn->ws = NULL;
    conn->reactor = reactor;
    atomic_init(&conn->refs, 1); /* owned by the protocol layer until teardown */
    conn->init_done = false;
    conn->dead = false;
    conn->pending = NULL;
    conn->pending_count = 0;
    conn->pending_cap = 0;
    return conn;
}

/* Shared by on_close (teardown) and the attach-failure path: mark dead, purge
 * broker operations and the pending queue, release the protocol reference.
 * Flush nodes still in flight keep the state alive and no-op on dispatch. */
static void gql_ws_conn_dispose(gql_ws_conn_t *conn) {
    pthread_mutex_lock(&g_broker_lock);
    conn->dead = true;
    conn->ws = NULL;
    gql_ws_conn_purge_ops(conn);
    for (size_t i = 0; i < conn->pending_count; i++) cwist_free(conn->pending[i]);
    conn->pending_count = 0;
    pthread_mutex_unlock(&g_broker_lock);
    gql_ws_conn_unref(conn);
}

/* WS layer teardown hook (reactor thread). */
static void gql_ws_on_close(cwist_websocket_async *ws, void *user_data) {
    (void)ws;
    gql_ws_conn_dispose((gql_ws_conn_t *)user_data);
}

static void gql_ws_on_message(cwist_websocket_async *ws, cwist_ws_frame *frame, void *user_data) {
    gql_ws_conn_t *conn = (gql_ws_conn_t *)user_data;
    /* CLOSE is handled by the WS layer itself (echo + teardown); PING already
     * got its PONG. Binary frames carry no graphql-ws semantics. */
    if (frame->opcode != CWIST_WS_FRAME_TEXT) {
        cwist_websocket_frame_destroy(frame);
        return;
    }
    cJSON *msg = cJSON_Parse((const char *)frame->payload);
    cwist_websocket_frame_destroy(frame);
    if (!msg) {
        cwist_websocket_async_close_code(ws, GQL_WS_CLOSE_BAD_REQUEST);
        return;
    }
    gql_ws_dispatch(ws, conn, msg);
    cJSON_Delete(msg);
}

bool cwist_graphql_ws_attach(cwist_graphql_ws_t *gws, int fd, cwist_reactor_t *reactor,
                             const uint8_t *initial, size_t initial_len) {
    if (!gws || fd < 0 || !reactor) return false;

    gql_ws_conn_t *conn = gql_ws_conn_create(gws, reactor);
    if (!conn) {
        close(fd);
        return false;
    }

    /* on_close releases the protocol state at teardown. */
    cwist_websocket_async *ws = cwist_websocket_async_attach_ex(
        fd, reactor, gql_ws_on_message, gql_ws_on_close, conn, initial, initial_len);
    if (!ws) {
        /* Attach failed after any inline initial-bytes delivery; no on_close
         * will ever run, so clean up here. */
        gql_ws_conn_dispose(conn);
        return false;
    }

    pthread_mutex_lock(&g_broker_lock);
    conn->ws = ws;
    pthread_mutex_unlock(&g_broker_lock);

    /* Drain anything queued by inline initial-bytes delivery. */
    gql_ws_post_flush(conn);
    return true;
}
