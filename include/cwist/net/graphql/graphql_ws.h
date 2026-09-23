/** @file graphql_ws.h @brief Experimental GraphQL subscriptions over the graphql-ws
 * WebSocket subprotocol (v3.7 Phase 4).
 *
 * Implements the `graphql-transport-ws` subprotocol on top of the non-blocking
 * WebSocket transport (cwist/net/websocket/websocket_async.h): connection_init ->
 * connection_ack, subscribe -> one `next` message per published event -> `complete`
 * on unsubscribe, `error` on operation failure, and `ping`/`pong` keepalive at any
 * time after init.
 *
 * Subscription fields resolve through the same schema/resolver model as queries and
 * mutations: a subscription resolver returns an event source (a set of broker topics)
 * and application code fans events out with cwist_graphql_publish(). The broker is
 * in-process and thread-safe: publish may be called from any thread; delivery to the
 * wire happens on the owning reactor thread.
 *
 * EXPERIMENTAL: API and wire behavior may change without notice in a future release.
 * Opt-in only; no existing behavior changes when unused. Requires the C1M
 * (callback-shaped) WebSocket path.
 */
#ifndef CWIST_NET_GRAPHQL_WS_H
#define CWIST_NET_GRAPHQL_WS_H

#include <cwist/net/graphql/graphql.h>
#include <cwist/sys/io/reactor.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_graphql_ws cwist_graphql_ws_t;
typedef struct cwist_graphql_event_source cwist_graphql_event_source_t;

/**
 * Subscription field resolver. Invoked once per incoming `subscribe` operation
 * (on the reactor thread).
 * @param args Parsed root-field arguments (cJSON object) or NULL when none. Borrowed:
 *             do not store or free.
 * @param variables The subscribe message's `variables` member (borrowed, may be NULL).
 * @param ctx Opaque pointer registered with cwist_graphql_ws_add_subscription().
 * @return A newly created event source expressing the topics this operation listens
 *         to, or NULL to reject the operation (the client receives an `error` message).
 */
typedef cwist_graphql_event_source_t *(*cwist_graphql_subscribe_fn)(const cJSON *args,
                                                                    const cJSON *variables,
                                                                    void *ctx);

/** Create a graphql-ws endpoint bound to @p schema. The schema is not owned and must
 *  outlive the endpoint. */
cwist_graphql_ws_t *cwist_graphql_ws_create(cwist_graphql_schema_t *schema);

/** Destroy an endpoint and free all subscription field registrations. Must be called
 *  only after every connection attached to the endpoint has been torn down (i.e. after
 *  the owning reactors have stopped); active connections keep the endpoint alive from
 *  the protocol layer's point of view and must be gone first. */
void cwist_graphql_ws_destroy(cwist_graphql_ws_t *gws);

/** Register or replace a subscription field resolver on the endpoint. */
bool cwist_graphql_ws_add_subscription(cwist_graphql_ws_t *gws, const char *field,
                                       cwist_graphql_subscribe_fn subscribe_fn, void *ctx);

/** Attach the graphql-ws protocol handler to a connected, upgraded fd. The fd is driven
 *  reactively by @p reactor (same ownership contract as cwist_websocket_async_attach):
 *  the HTTP layer must not touch the fd after a successful call.
 * @param gws Endpoint whose schema resolves subscriptions.
 * @param fd Connected client socket (set non-blocking here).
 * @param reactor Reactor that will own the connection.
 * @param initial Bytes already read past the upgrade request (may be NULL).
 * @param initial_len Number of valid bytes at @p initial.
 * @return true when the fd was handed to the reactor path, false on failure (fd closed). */
bool cwist_graphql_ws_attach(cwist_graphql_ws_t *gws, int fd, cwist_reactor_t *reactor,
                             const uint8_t *initial, size_t initial_len);

/** Create an empty event source for use as a subscription resolver's return value. */
cwist_graphql_event_source_t *cwist_graphql_event_source_create(void);

/** Register interest in a broker topic. Events published to @p topic are delivered to
 *  every active subscription whose event source lists it. Returns false on invalid
 *  input or allocation failure. */
bool cwist_graphql_event_source_add_topic(cwist_graphql_event_source_t *src, const char *topic);

/** Destroy an event source. Called automatically for active subscriptions when their
 *  operation ends (client `complete`, socket close) or the endpoint is destroyed. */
void cwist_graphql_event_source_destroy(cwist_graphql_event_source_t *src);

/** Publish an event to the in-process broker. The payload becomes the field value inside
 *  each delivered `next` message's `data` (`null` when @p payload is NULL). Thread-safe:
 *  may be called from any thread; matched subscriptions are queued and delivered on their
 *  connection's reactor thread.
 * @param topic Broker topic (non-empty string).
 * @param payload Event payload (borrowed) or NULL.
 * @return Number of `next` messages queued. */
size_t cwist_graphql_publish(const char *topic, const cJSON *payload);

#ifdef __cplusplus
}
#endif
#endif
