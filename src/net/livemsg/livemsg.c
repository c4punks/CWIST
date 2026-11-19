/** @file livemsg.c
 * @brief Experimental transport-independent message dispatch runtime.
 */
#include <cwist/livemsg.h>

#include <cwist/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/arena.h>
#include <cwist/net/webrtc.h>
#include <cwist/net/websocket/websocket_async.h>

#include "../../sys/app/app_route_internal.h"
#include "../websocket/ws_async_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LIVEMSG_ARENA_BYTES (2u * CWIST_LIVEMSG_MAX_MESSAGE_BYTES + 4096u)
#define LIVEMSG_MAX_SDP_BYTES (64u * 1024u)
#define LIVEMSG_WS_FRAME_OVERHEAD 10u

struct cwist_livemsg_reply {
    uint8_t *data;
    size_t len;
    size_t cap;
};

struct cwist_livemsg_peer {
    cwist_livemsg_ctx *ctx;
    cwist_websocket_async *ws;
    cwist_webrtc_conn *rtc;
    uint16_t channel;
    cwist_arena_t *message_arena;
    cwist_livemsg_peer *next;
};

struct cwist_livemsg_ctx {
    cwist_livemsg_config config;
    cwist_app *app;
    cwist_webrtc_ctx *rtc;
    cwist_livemsg_peer *peers;
};

static void peer_destroy(cwist_livemsg_peer *peer) {
    if (!peer) return;
    cwist_livemsg_ctx *ctx = peer->ctx;
    cwist_livemsg_peer **link = &ctx->peers;
    while (*link && *link != peer) link = &(*link)->next;
    if (*link) *link = peer->next;
    if (ctx->config.on_disconnect) ctx->config.on_disconnect(peer, ctx->config.user_data);
#ifdef CWIST_WEBRTC
    if (peer->rtc) cwist_webrtc_conn_release(peer->rtc);
#endif
    cwist_free(peer);
}

#if defined(CWIST_WS) || defined(CWIST_WEBRTC)
static cwist_livemsg_peer *peer_create(cwist_livemsg_ctx *ctx) {
    cwist_livemsg_peer *peer = cwist_alloc(sizeof(*peer));
    if (!peer) return NULL;
    peer->ctx = ctx;
    peer->next = ctx->peers;
    ctx->peers = peer;
    return peer;
}
#endif

static int peer_send(cwist_livemsg_peer *peer, const void *data, size_t len, bool text) {
    (void)text;
    if (!peer || (len && !data)) return -EINVAL;
    if (len > CWIST_LIVEMSG_MAX_MESSAGE_BYTES) return -EMSGSIZE;
#ifdef CWIST_WS
    if (peer->ws) {
        size_t queued = cwist_websocket_async_buffered_amount(peer->ws);
        size_t framed = len + LIVEMSG_WS_FRAME_OVERHEAD;
        if (queued > CWIST_LIVEMSG_MAX_BUFFERED_BYTES ||
            framed > CWIST_LIVEMSG_MAX_BUFFERED_BYTES - queued)
            return -EAGAIN;
        int rc = cwist_websocket_async_send(
            peer->ws, text ? CWIST_WS_FRAME_TEXT : CWIST_WS_FRAME_BINARY, data, len);
        if (rc == CWIST_WEBSOCKET_ASYNC_SEND_BACKPRESSURE) return -EAGAIN;
        return rc == 0 ? 0 : -EIO;
    }
#endif
#ifdef CWIST_WEBRTC
    if (peer->rtc) {
        size_t queued = cwist_webrtc_conn_buffered_amount(peer->rtc);
        if (queued > CWIST_LIVEMSG_MAX_BUFFERED_BYTES ||
            len > CWIST_LIVEMSG_MAX_BUFFERED_BYTES - queued)
            return -EAGAIN;
        int rc = cwist_webrtc_conn_send(peer->rtc, peer->channel, data, len,
                                        text ? CWIST_WEBRTC_DATA_STRING : CWIST_WEBRTC_DATA_BINARY);
        return rc == 0 ? 0 : -EIO;
    }
#endif
    return -ENOTSUP;
}

int cwist_livemsg_send_raw(cwist_livemsg_peer *peer, const void *data, size_t len) {
    return peer_send(peer, data, len, false);
}

int cwist_livemsg_send_json(cwist_livemsg_peer *peer, const cJSON *root) {
    if (!peer || !root) return -EINVAL;
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return -ENOMEM;
    int rc = peer_send(peer, json, strlen(json), true);
    cJSON_free(json);
    return rc;
}

void cwist_livemsg_foreach_peer(cwist_livemsg_ctx *ctx,
                                void (*fn)(cwist_livemsg_peer *peer, void *arg), void *arg) {
    if (!ctx || !fn) return;
    for (cwist_livemsg_peer *peer = ctx->peers; peer; peer = peer->next) fn(peer, arg);
}

cwist_livemsg_ctx *cwist_livemsg_peer_ctx(cwist_livemsg_peer *peer) {
    return peer ? peer->ctx : NULL;
}

void *cwist_livemsg_alloc(cwist_livemsg_peer *peer, size_t size) {
    if (!peer || !peer->message_arena || !size) return NULL;
    return cwist_arena_alloc(peer->message_arena, size);
}

int cwist_livemsg_reply_write(cwist_livemsg_reply *reply, const void *data, size_t len) {
    if (!reply || (len && !data)) return -EINVAL;
    if (len > CWIST_LIVEMSG_MAX_MESSAGE_BYTES - reply->len) return -EMSGSIZE;
    if (!len) return 0;
    size_t needed = reply->len + len;
    if (needed > reply->cap) {
        size_t cap = reply->cap ? reply->cap : 256;
        while (cap < needed) cap *= 2;
        uint8_t *next = cwist_realloc(reply->data, cap);
        if (!next) return -ENOMEM;
        reply->data = next;
        reply->cap = cap;
    }
    memcpy(reply->data + reply->len, data, len);
    reply->len = needed;
    return 0;
}

int cwist_livemsg_reply_set_json(cwist_livemsg_reply *reply, cJSON *root) {
    if (!reply || !root) return -EINVAL;
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return -ENOMEM;
    int rc = cwist_livemsg_reply_write(reply, json, strlen(json));
    cJSON_free(json);
    return rc;
}

#if defined(CWIST_WS) || defined(CWIST_WEBRTC)
static void dispatch(cwist_livemsg_peer *peer, const uint8_t *data, size_t len) {
    cwist_livemsg_ctx *ctx = peer->ctx;
    if (len > CWIST_LIVEMSG_MAX_MESSAGE_BYTES) {
#ifdef CWIST_WS
        if (peer->ws) cwist_websocket_async_close_code(peer->ws, 1009);
#endif
        return;
    }
    cwist_arena_t *arena = cwist_arena_create(LIVEMSG_ARENA_BYTES);
    if (!arena) return;
    uint8_t *input = cwist_arena_alloc(arena, len + 1);
    if (!input) {
        cwist_arena_destroy(arena);
        return;
    }
    if (len) memcpy(input, data, len);
    input[len] = 0;
    peer->message_arena = arena;

    cJSON *json = NULL;
    const void *msg = input;
    if (ctx->config.codec == CWIST_LIVEMSG_CODEC_JSON) {
        json = cJSON_ParseWithLengthOpts((const char *)input, len + 1, NULL, true);
        if (!json) {
#ifdef CWIST_WS
            if (peer->ws) cwist_websocket_async_close_code(peer->ws, 1007);
#endif
            goto done;
        }
        msg = json;
    }

    void *result = NULL;
    if (ctx->config.process(peer, msg, len, &result, ctx->config.user_data) != CWIST_LIVEMSG_OK)
        goto done;
    cwist_livemsg_reply reply = {0};
    int rc = ctx->config.respond(peer, result, &reply, ctx->config.user_data);
    if (rc == CWIST_LIVEMSG_OK && reply.len)
        (void)peer_send(peer, reply.data, reply.len, ctx->config.codec == CWIST_LIVEMSG_CODEC_JSON);
    cwist_free(reply.data);

done:
    peer->message_arena = NULL;
    if (json) cJSON_Delete(json);
    cwist_arena_destroy(arena);
}
#endif

#ifdef CWIST_WS
static void ws_on_open(cwist_websocket_async *ws, void *user) {
    cwist_livemsg_peer *peer = peer_create(user);
    if (!peer) {
        cwist_websocket_async_close_code(ws, 1011);
        return;
    }
    peer->ws = ws;
    cwist_livemsg_ctx *ctx = user;
    if (ctx->config.on_connect) ctx->config.on_connect(peer, ctx->config.user_data);
}

static cwist_livemsg_peer *find_ws_peer(cwist_livemsg_ctx *ctx, cwist_websocket_async *ws) {
    for (cwist_livemsg_peer *peer = ctx->peers; peer; peer = peer->next) {
        if (peer->ws == ws) return peer;
    }
    return NULL;
}

static void ws_on_message(cwist_websocket_async *ws, cwist_ws_frame *frame, void *user) {
    cwist_livemsg_peer *peer = find_ws_peer(user, ws);
    if (peer) dispatch(peer, frame->payload, frame->payload_len);
    cwist_websocket_frame_destroy(frame);
}

static void ws_on_close(cwist_websocket_async *ws, void *user) {
    cwist_livemsg_peer *peer = find_ws_peer(user, ws);
    if (peer) {
        peer->ws = NULL;
        peer_destroy(peer);
    }
}
#endif

#ifdef CWIST_WEBRTC
static cwist_livemsg_peer *find_rtc_peer(cwist_livemsg_ctx *ctx, cwist_webrtc_conn *conn,
                                         uint16_t channel) {
    for (cwist_livemsg_peer *peer = ctx->peers; peer; peer = peer->next) {
        if (peer->rtc == conn && peer->channel == channel) return peer;
    }
    return NULL;
}

static void rtc_on_channel(cwist_webrtc_conn *conn, uint16_t channel, const char *label,
                           void *user) {
    (void)label;
    if (find_rtc_peer(user, conn, channel)) return;
    cwist_livemsg_peer *peer = peer_create(user);
    if (!peer) {
        cwist_webrtc_conn_close(conn);
        return;
    }
    peer->rtc = conn;
    peer->channel = channel;
    cwist_webrtc_conn_retain(conn);
    cwist_livemsg_ctx *ctx = user;
    if (ctx->config.on_connect) ctx->config.on_connect(peer, ctx->config.user_data);
}

static void rtc_on_message(cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type, void *user) {
    (void)type;
    cwist_livemsg_peer *peer = find_rtc_peer(user, conn, channel);
    if (peer) dispatch(peer, data, len);
}

static void rtc_on_close(cwist_webrtc_conn *conn, void *user) {
    cwist_livemsg_ctx *ctx = user;
    cwist_livemsg_peer *peer = ctx->peers;
    while (peer) {
        cwist_livemsg_peer *next = peer->next;
        if (peer->rtc == conn) peer_destroy(peer);
        peer = next;
    }
}
#endif

static void config_free(cwist_livemsg_config *config) {
    cwist_free((char *)config->ws_path);
    cwist_free((char *)config->signaling_path);
    config->ws_path = NULL;
    config->signaling_path = NULL;
}

static bool config_copy(cwist_livemsg_ctx *ctx, const cwist_livemsg_config *config) {
    if (!config || !config->process || !config->respond) {
        errno = EINVAL;
        return false;
    }
    if ((config->transport != CWIST_LIVEMSG_TRANSPORT_WS &&
         config->transport != CWIST_LIVEMSG_TRANSPORT_WEBRTC) ||
        (config->codec != CWIST_LIVEMSG_CODEC_RAW && config->codec != CWIST_LIVEMSG_CODEC_JSON)) {
        errno = ENOTSUP;
        return false;
    }
    ctx->config = *config;
    ctx->config.ws_path = cwist_strdup(config->ws_path ? config->ws_path : "/");
    ctx->config.signaling_path =
        config->signaling_path ? cwist_strdup(config->signaling_path) : NULL;
    if (!ctx->config.ws_path || (config->signaling_path && !ctx->config.signaling_path)) {
        config_free(&ctx->config);
        return false;
    }
    return true;
}

#ifdef CWIST_WEBRTC
static void signaling_route_handler(void *user, cwist_http_request *req, cwist_http_response *res) {
    cwist_livemsg_ctx *ctx = user;
    char *answer = NULL;
    if (!req->body || !req->body->data || req->body->size > LIVEMSG_MAX_SDP_BYTES ||
        cwist_livemsg_webrtc_offer(ctx, req->body->data, &answer) < 0) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "invalid SDP offer");
        return;
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/sdp");
    cwist_sstring_assign(res->body, answer);
    cwist_free(answer);
}
#endif

void cwist_livemsg_config_init(cwist_livemsg_config *config, uint32_t transport) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->transport = transport;
    config->ws_path = "/";
    config->signaling_path = transport == CWIST_LIVEMSG_TRANSPORT_WEBRTC ? "/signal" : NULL;
}

cwist_livemsg_ctx *cwist_livemsg_create(const cwist_livemsg_config *config) {
    cwist_livemsg_ctx *ctx = cwist_alloc(sizeof(*ctx));
    if (!ctx) return NULL;
    if (!config_copy(ctx, config)) {
        int error = errno;
        cwist_free(ctx);
        errno = error;
        return NULL;
    }
    ctx->app = cwist_app_create();
    if (!ctx->app) {
        config_free(&ctx->config);
        cwist_free(ctx);
        return NULL;
    }
    bool ok = false;
    if (config->transport == CWIST_LIVEMSG_TRANSPORT_WS) {
#ifdef CWIST_WS
        ok = cwist_app_internal_ws_async(ctx->app, ctx->config.ws_path, ws_on_message, ws_on_open,
                                         ws_on_close, ctx, CWIST_LIVEMSG_MAX_MESSAGE_BYTES);
#else
        errno = ENOTSUP;
#endif
    } else {
#ifdef CWIST_WEBRTC
        ctx->rtc = cwist_webrtc_ctx_new(config->udp_port);
        if (ctx->rtc) {
            cwist_webrtc_ctx_set_message_handler(ctx->rtc, rtc_on_message, ctx);
            cwist_webrtc_ctx_set_channel_handler(ctx->rtc, rtc_on_channel, ctx);
            cwist_webrtc_ctx_set_close_handler(ctx->rtc, rtc_on_close, ctx);
            ok = true;
            if (ctx->config.signaling_path) {
                cwist_error_t err = cwist_app_post_ex(ctx->app, ctx->config.signaling_path,
                                                      signaling_route_handler, ctx, NULL);
                ok = err.error.err_i16 == 0;
            }
        }
#else
        errno = ENOTSUP;
#endif
    }
    if (!ok) {
        int error = errno;
        cwist_livemsg_destroy(ctx);
        errno = error;
        return NULL;
    }
    return ctx;
}

int cwist_livemsg_run(cwist_livemsg_ctx *ctx) {
    if (!ctx || !ctx->app) return -EINVAL;
    return cwist_app_listen_ex(ctx->app, ctx->config.port, 1, 1);
}

void cwist_livemsg_destroy(cwist_livemsg_ctx *ctx) {
    if (!ctx) return;
#ifdef CWIST_WEBRTC
    if (ctx->rtc) cwist_webrtc_ctx_free(ctx->rtc);
#endif
    while (ctx->peers) peer_destroy(ctx->peers);
    if (ctx->app) cwist_app_destroy(ctx->app);
    config_free(&ctx->config);
    cwist_free(ctx);
}

static int serve_config(const cwist_livemsg_config *config) {
    cwist_livemsg_ctx *ctx = cwist_livemsg_create(config);
    if (!ctx) return errno ? -errno : -EINVAL;
    int rc = cwist_livemsg_run(ctx);
    cwist_livemsg_destroy(ctx);
    return rc;
}

int cwist_livemsg_serve(cwist_livemsg_config_fn make_config) {
    if (!make_config) return -EINVAL;
    cwist_livemsg_config config = make_config();
    return serve_config(&config);
}

int cwist_livemsg_serve_args(int argc, char **argv,
                             cwist_livemsg_config (*make_config)(int, char **)) {
    if (!make_config) return -EINVAL;
    cwist_livemsg_config config = make_config(argc, argv);
    return serve_config(&config);
}

int cwist_livemsg_webrtc_offer(cwist_livemsg_ctx *ctx, const char *offer_sdp,
                               char **answer_sdp_out) {
    if (!answer_sdp_out) return -EINVAL;
    *answer_sdp_out = NULL;
    if (!ctx || ctx->config.transport != CWIST_LIVEMSG_TRANSPORT_WEBRTC || !offer_sdp)
        return -EINVAL;
#ifdef CWIST_WEBRTC
    char answer[4096];
    if (!ctx->rtc || cwist_webrtc_handle_offer(ctx->rtc, offer_sdp, answer, sizeof(answer)) < 0)
        return -EIO;
    *answer_sdp_out = cwist_strdup(answer);
    return *answer_sdp_out ? 0 : -ENOMEM;
#else
    return -ENOTSUP;
#endif
}
