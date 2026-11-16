/**
 * @file livemsg.h
 * @brief Experimental message runtime over WebSocket and WebRTC DataChannel.
 *
 * One peer represents one WebSocket connection or one WebRTC DataChannel.
 * Callbacks run on the transport's reactor thread. Input, decoded JSON, and
 * allocations made with cwist_livemsg_alloc() remain valid until respond()
 * returns. A peer pointer must not be retained after on_disconnect().
 */
#ifndef __CWIST_LIVEMSG_H__
#define __CWIST_LIVEMSG_H__

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CWIST_LIVEMSG_MAX_MESSAGE_BYTES (256u * 1024u)
#define CWIST_LIVEMSG_MAX_BUFFERED_BYTES (4u * 1024u * 1024u)

typedef enum {
    CWIST_LIVEMSG_TRANSPORT_WS = 1 << 0,
    CWIST_LIVEMSG_TRANSPORT_WEBRTC = 1 << 1,
} cwist_livemsg_transport_t;

typedef enum {
    CWIST_LIVEMSG_CODEC_RAW = 0,
    CWIST_LIVEMSG_CODEC_JSON = 1,
} cwist_livemsg_codec_t;

enum {
    CWIST_LIVEMSG_OK = 0,
    CWIST_LIVEMSG_NO_REPLY = 1,
};

typedef struct cwist_livemsg_peer cwist_livemsg_peer;
typedef struct cwist_livemsg_ctx cwist_livemsg_ctx;
typedef struct cwist_livemsg_reply cwist_livemsg_reply;

typedef struct {
    uint32_t transport;
    cwist_livemsg_codec_t codec;
    uint16_t port;
    uint16_t udp_port;
    const char *ws_path;
    const char *signaling_path;

    int (*process)(cwist_livemsg_peer *peer, const void *msg, size_t len, void **result,
                   void *user_data);
    int (*respond)(cwist_livemsg_peer *peer, void *result, cwist_livemsg_reply *reply,
                   void *user_data);

    void (*on_connect)(cwist_livemsg_peer *peer, void *user_data);
    void (*on_disconnect)(cwist_livemsg_peer *peer, void *user_data);
    void *user_data;
} cwist_livemsg_config;

typedef cwist_livemsg_config (*cwist_livemsg_config_fn)(void);

/** Initialize defaults; set signaling_path to NULL to disable WebRTC signaling. */
void cwist_livemsg_config_init(cwist_livemsg_config *config, uint32_t transport);

/** Create a runtime without starting its HTTP listener. */
cwist_livemsg_ctx *cwist_livemsg_create(const cwist_livemsg_config *config);

/** Run the configured listener in one reactor process until shutdown. */
int cwist_livemsg_run(cwist_livemsg_ctx *ctx);

/** Stop transport contexts and release the runtime after cwist_livemsg_run(). */
void cwist_livemsg_destroy(cwist_livemsg_ctx *ctx);

/** Serve a configuration returned by value. */
int cwist_livemsg_serve(cwist_livemsg_config_fn make_config);
int cwist_livemsg_serve_args(int argc, char **argv,
                             cwist_livemsg_config (*make_config)(int, char **));

/** Return the runtime owning this peer for broadcast and external signaling. */
cwist_livemsg_ctx *cwist_livemsg_peer_ctx(cwist_livemsg_peer *peer);

/** Allocate storage in the current message arena. */
void *cwist_livemsg_alloc(cwist_livemsg_peer *peer, size_t size);

/** Append a reply, or serialize a borrowed cJSON tree. */
int cwist_livemsg_reply_write(cwist_livemsg_reply *reply, const void *data, size_t len);
int cwist_livemsg_reply_set_json(cwist_livemsg_reply *reply, cJSON *root);

/** Queue a binary or JSON message without blocking the reactor. */
int cwist_livemsg_send_raw(cwist_livemsg_peer *peer, const void *data, size_t len);
int cwist_livemsg_send_json(cwist_livemsg_peer *peer, const cJSON *root);

/** Visit connected peers on the calling transport's reactor thread. */
void cwist_livemsg_foreach_peer(cwist_livemsg_ctx *ctx,
                                void (*fn)(cwist_livemsg_peer *peer, void *arg), void *arg);

/** Process an external SDP offer. Free *answer_sdp_out with cwist_free(). */
int cwist_livemsg_webrtc_offer(cwist_livemsg_ctx *ctx, const char *offer_sdp,
                               char **answer_sdp_out);

#ifdef __cplusplus
}
#endif

#endif
