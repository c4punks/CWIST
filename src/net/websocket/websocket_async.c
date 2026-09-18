#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/**
 * @file websocket_async.c
 * @brief Reactor-driven, callback-shaped non-blocking WebSocket path (C1M mode).
 *
 * Issue #181: on the C1M path the route handler runs on the reactor thread,
 * so the blocking recv() loop of cwist_websocket_receive() would park the
 * entire worker.  This module instead registers the upgraded fd with the
 * reactor, drains it with non-blocking recv() calls, parses frames
 * incrementally from a grow-by-doubling stash buffer (same model as the HTTP
 * async connection rbuf), and delivers each complete message through the
 * route's on_message callback.
 *
 * Frame validation mirrors cwist_websocket_receive() rule-for-rule so both
 * paths enforce identical RFC 6455 semantics (issues #145-#164).
 */

#include <cwist/net/websocket/websocket_async.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/writer_fast.h>
#include <cwist/sys/io/reactor.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Same caps as websocket.c: per-frame 16 MiB, reassembled message 64 MiB. */
#define WS_ASYNC_MAX_PAYLOAD_BYTES ((uint64_t)(16u * 1024u * 1024u))
#define WS_ASYNC_MAX_MESSAGE_BYTES ((size_t)(64u * 1024u * 1024u))

/* Absolute parked-write deadline budget (seconds), mirroring the HTTP
 * parked-response writer's progress-reset + absolute-deadline scheme. */
#define WS_ASYNC_PARK_DEADLINE_SEC 30u

#define WS_ASYNC_READ_CHUNK 16384u

/* Reactor slot payload: just the connection pointer. */
typedef struct {
    cwist_websocket_async *ws;
} ws_async_slot_t;

_Static_assert(sizeof(ws_async_slot_t) <= CWIST_REACTOR_PAYLOAD_SIZE,
               "ws async slot state must fit a reactor slot payload");

struct cwist_websocket_async {
    int fd;
    cwist_reactor_t *reactor;
    cwist_ws_on_message_t on_message;
    void *user_data;

    /* Read stash: raw bytes not yet consumed by the incremental parser. */
    uint8_t *stash;
    size_t stash_len;
    size_t stash_cap;

    /* Fragmented-message reassembly state (RFC 6455 section 5.4), same
     * shape as cwist_websocket in websocket.h. */
    uint8_t *frag_buf;
    size_t frag_len;
    size_t frag_cap;
    cwist_ws_opcode_t frag_opcode;

    /* Parked write: owned buffer + offset, resumed by a one-shot POLLOUT slot
     * (model: http_parked_write_t in src/net/http/http.c). */
    uint8_t *park_buf;
    size_t park_off;
    size_t park_len;
    uint32_t park_deadline_sec;

    bool closed;            /* CLOSE handshake initiated (send side). */
    bool terminate_pending; /* Connection must be torn down this turn. */
    bool read_armed;        /* A one-shot read slot is registered. */
};

static void ws_async_read_ready(int fd, void *ctx);
static void ws_async_write_ready(int fd, void *ctx);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Tear down the connection: cancel armed reactor slots, close the fd and
 * release all state.  Single owner of destruction; every exit path funnels
 * through here, so it must be called at most once per connection. */
static void ws_async_terminate(cwist_websocket_async *ws) {
    if (!ws) return;
    if (ws->fd >= 0) {
        cwist_reactor_del(ws->reactor, ws->fd);
        close(ws->fd);
        ws->fd = -1;
    }
    cwist_free(ws->stash);
    cwist_free(ws->frag_buf);
    cwist_free(ws->park_buf);
    cwist_free(ws);
}

static uint32_t ws_async_park_deadline(void) {
    return cwist_fast_monotonic_sec() + WS_ASYNC_PARK_DEADLINE_SEC;
}

/* ------------------------------------------------------------------ */
/* Parked (POLLOUT-resumable) writes                                   */
/* ------------------------------------------------------------------ */

/* Serialize one FIN-terminated server frame (server frames are never
 * masked) into a fresh owned buffer.  Returns NULL on allocation failure. */
static uint8_t *ws_async_serialize_frame(cwist_ws_opcode_t opcode, const uint8_t *data, size_t len,
                                         size_t *out_total) {
    uint8_t head[10];
    size_t head_len = 2;

    head[0] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        head[1] = (uint8_t)len;
    } else if (len < 65536) {
        head[1] = 126;
        head[2] = (uint8_t)((len >> 8) & 0xFF);
        head[3] = (uint8_t)(len & 0xFF);
        head_len += 2;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; i++) head[2 + i] = (uint8_t)(len >> ((7 - i) * 8));
        head_len += 8;
    }

    uint8_t *buf = (uint8_t *)cwist_alloc(head_len + len);
    if (!buf) return NULL;
    memcpy(buf, head, head_len);
    if (len > 0) memcpy(buf + head_len, data, len);
    *out_total = head_len + len;
    return buf;
}

/* Queue raw frame bytes behind any already-parked unsent bytes, then drain
 * speculatively; whatever does not fit in the socket buffer is parked on a
 * one-shot POLLOUT slot.  Returns 0 when the frame is sent or parked, -1 on
 * failure (connection is terminated by the caller). */
static int ws_async_queue_frame(cwist_websocket_async *ws, cwist_ws_opcode_t opcode,
                                const uint8_t *data, size_t len) {
    size_t frame_len = 0;
    uint8_t *frame = ws_async_serialize_frame(opcode, data, len, &frame_len);
    if (!frame) return -1;

    size_t pending = ws->park_len - ws->park_off;
    if (pending > 0) {
        /* Append behind the unsent remainder, compacting to the front. */
        uint8_t *nb = (uint8_t *)cwist_alloc(pending + frame_len);
        if (!nb) {
            cwist_free(frame);
            return -1;
        }
        memcpy(nb, ws->park_buf + ws->park_off, pending);
        memcpy(nb + pending, frame, frame_len);
        cwist_free(ws->park_buf);
        ws->park_buf = nb;
        ws->park_off = 0;
        ws->park_len = pending + frame_len;
    } else {
        cwist_free(ws->park_buf);
        ws->park_buf = frame;
        ws->park_off = 0;
        ws->park_len = frame_len;
        frame = NULL;
    }
    cwist_free(frame);

    int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
    while (ws->park_off < ws->park_len) {
        ssize_t n = send(ws->fd, ws->park_buf + ws->park_off, ws->park_len - ws->park_off, flags);
        if (n > 0) {
            ws->park_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1; /* Fatal send error. */
    }

    if (ws->park_off == ws->park_len) {
        /* Fully drained. */
        cwist_free(ws->park_buf);
        ws->park_buf = NULL;
        ws->park_off = ws->park_len = 0;
        return 0;
    }

    ws->park_deadline_sec = ws_async_park_deadline();
    ws_async_slot_t slot = {.ws = ws};
    if (!cwist_reactor_add_out(ws->reactor, ws->fd, ws_async_write_ready, &slot, sizeof(slot)))
        return -1;
    return 0;
}

static void ws_async_write_ready(int fd, void *ctx) {
    ws_async_slot_t *slot = (ws_async_slot_t *)ctx;
    cwist_websocket_async *ws = slot->ws;
    int flags = MSG_NOSIGNAL | MSG_DONTWAIT;

    while (ws->park_off < ws->park_len) {
        ssize_t n = send(fd, ws->park_buf + ws->park_off, ws->park_len - ws->park_off, flags);
        if (n > 0) {
            ws->park_off += (size_t)n;
            /* Progress resets the budget so a slow-but-alive client can drain. */
            ws->park_deadline_sec = ws_async_park_deadline();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            cwist_fast_monotonic_sec() <= ws->park_deadline_sec &&
            cwist_reactor_add_out(ws->reactor, fd, ws_async_write_ready, slot, sizeof(*slot))) {
            /* Payload copied into the new slot; buffer ownership moves with it. */
            return;
        }
        ws_async_terminate(ws);
        return;
    }

    cwist_free(ws->park_buf);
    ws->park_buf = NULL;
    ws->park_off = ws->park_len = 0;

    if (ws->terminate_pending) {
        ws_async_terminate(ws);
        return;
    }
    /* Re-arm the read side only when the read turn is not already armed. */
    if (!ws->read_armed) {
        ws_async_slot_t rslot = {.ws = ws};
        if (cwist_reactor_add(ws->reactor, fd, ws_async_read_ready, &rslot, sizeof(rslot)))
            ws->read_armed = true;
        else
            ws_async_terminate(ws);
    }
}

int cwist_websocket_async_send(cwist_websocket_async *ws, cwist_ws_opcode_t opcode,
                               const uint8_t *data, size_t len) {
    if (!ws || ws->closed || (!data && len > 0)) return -1;
    return ws_async_queue_frame(ws, opcode, data, len);
}

void cwist_websocket_async_close(cwist_websocket_async *ws) {
    if (!ws || ws->closed) return;
    ws->closed = true;
    /* Best-effort CLOSE frame; failure still tears the connection down. */
    if (ws_async_queue_frame(ws, CWIST_WS_FRAME_CLOSE, NULL, 0) != 0) {
        ws->terminate_pending = true;
        return;
    }
    if (ws->park_off == ws->park_len) {
        /* Nothing parked: the CLOSE frame is on the wire; tear down now.  The
         * enclosing read turn (we run on the reactor thread) performs the
         * actual free via terminate_pending. */
        ws->terminate_pending = true;
    }
    /* Otherwise the parked-write completion terminates after draining. */
}

/* ------------------------------------------------------------------ */
/* Incremental frame parsing (mirrors cwist_websocket_receive)         */
/* ------------------------------------------------------------------ */

static bool ws_async_stash_append(cwist_websocket_async *ws, const uint8_t *src, size_t len) {
    if (len == 0) return true;
    size_t need = ws->stash_len + len;
    if (need > ws->stash_cap) {
        size_t new_cap = ws->stash_cap ? ws->stash_cap * 2 : 4096;
        if (new_cap < need) new_cap = need;
        uint8_t *nb = (uint8_t *)cwist_alloc(new_cap);
        if (!nb) return false;
        if (ws->stash_len) memcpy(nb, ws->stash, ws->stash_len);
        cwist_free(ws->stash);
        ws->stash = nb;
        ws->stash_cap = new_cap;
    }
    memcpy(ws->stash + ws->stash_len, src, len);
    ws->stash_len += len;
    return true;
}

/* Append len bytes of src to the reassembly buffer, growing by doubling.
 * Returns false on allocation failure or when the reassembled message would
 * exceed the total-message cap. */
static bool ws_async_frag_append(cwist_websocket_async *ws, const uint8_t *src, size_t len) {
    if (len == 0) return true;
    if (ws->frag_len + len > WS_ASYNC_MAX_MESSAGE_BYTES) return false;
    size_t need = ws->frag_len + len + 1; /* +1 for null terminator */
    if (need > ws->frag_cap) {
        size_t new_cap = ws->frag_cap ? ws->frag_cap * 2 : 4096;
        if (new_cap < need) new_cap = need;
        uint8_t *nb = (uint8_t *)cwist_alloc(new_cap);
        if (!nb) return false;
        if (ws->frag_len) memcpy(nb, ws->frag_buf, ws->frag_len);
        cwist_free(ws->frag_buf);
        ws->frag_buf = nb;
        ws->frag_cap = new_cap;
    }
    memcpy(ws->frag_buf + ws->frag_len, src, len);
    ws->frag_len += len;
    return true;
}

/* Handle one complete, unmasked frame.  Takes ownership of @p payload.
 * Returns 0 on success, -1 on a protocol violation (connection must fail). */
static int ws_async_process_frame(cwist_websocket_async *ws, bool fin, cwist_ws_opcode_t opcode,
                                  uint8_t *payload, size_t payload_len) {
    bool is_control = (opcode >= 0x8);

    /* --- Fragmented-message reassembly (RFC 6455 section 5.4) -------------- */
    if (!is_control && !fin) {
        /* Fragment with FIN=0: start or continue reassembly. */
        if (opcode == CWIST_WS_FRAME_CONTINUATION) {
            /* CONTINUATION is only valid when a fragmented message is
             * already in progress. */
            if (ws->frag_len == 0) {
                cwist_free(payload);
                return -1;
            }
        } else {
            /* First fragment: save opcode (text vs binary). */
            ws->frag_opcode = opcode;
        }
        if (!ws_async_frag_append(ws, payload, payload_len)) {
            cwist_free(payload);
            return -1;
        }
        cwist_free(payload);
        return 0;
    }

    /* A CONTINUATION frame is only valid inside an active fragmented-message
     * sequence; an orphan CONTINUATION with FIN=1 must not be delivered as a
     * complete message (mirrors the blocking path, issue #158). */
    if (!is_control && opcode == CWIST_WS_FRAME_CONTINUATION && ws->frag_len == 0) {
        cwist_free(payload);
        return -1;
    }

    /* FIN=1 data frame: may be the last fragment of a multi-frame message. */
    if (!is_control && ws->frag_len > 0) {
        if (!ws_async_frag_append(ws, payload, payload_len)) {
            cwist_free(payload);
            cwist_free(ws->frag_buf);
            ws->frag_buf = NULL;
            ws->frag_len = ws->frag_cap = 0;
            return -1;
        }
        cwist_free(payload);

        /* Hand ownership of the reassembly buffer to the delivered frame. */
        payload = ws->frag_buf;
        payload_len = ws->frag_len;
        payload[payload_len] = '\0';
        opcode = ws->frag_opcode;
        fin = true;

        ws->frag_buf = NULL;
        ws->frag_len = ws->frag_cap = 0;
    }

    /* --- Deliver the complete frame ---------------------------------- */
    if (opcode == CWIST_WS_FRAME_CLOSE) {
        /* RFC 6455 section 5.5.1: a CLOSE body, if present, MUST be >= 2 bytes. */
        if (payload_len == 1) {
            cwist_free(payload);
            return -1;
        }
        /* Echo the status code (first 2 bytes) back before closing. */
        ws_async_queue_frame(ws, CWIST_WS_FRAME_CLOSE, payload, (payload_len >= 2) ? 2 : 0);
        cwist_free(payload);
        ws->closed = true;
        ws->terminate_pending = true;
        return 0;
    }
    if (opcode == CWIST_WS_FRAME_PING) {
        /* RFC 6455 section 5.5.3: respond to every PING with a PONG carrying
         * the same payload (up to 125 bytes per section 5.5). */
        ws_async_queue_frame(ws, CWIST_WS_FRAME_PONG, payload, payload_len);
    }

    cwist_ws_frame *frame = (cwist_ws_frame *)cwist_alloc(sizeof(cwist_ws_frame));
    if (!frame) {
        cwist_free(payload);
        return -1;
    }
    frame->fin = fin;
    frame->opcode = opcode;
    frame->payload = payload;
    frame->payload_len = payload_len;
    /* Ownership moves to the callback; it must destroy the frame. */
    ws->on_message(ws, frame, ws->user_data);
    return 0;
}

/* Parse as many complete frames as the stash holds.  Partial frame bytes
 * stay in the stash for the next read.  Returns 0 on success, -1 on a
 * protocol violation. */
static int ws_async_feed(cwist_websocket_async *ws) {
    size_t off = 0;

    while (ws->stash_len - off >= 2) {
        const uint8_t *base = ws->stash + off;
        bool fin = (base[0] & 0x80) != 0;
        cwist_ws_opcode_t opcode = base[0] & 0x0F;

        /* RFC 6455 section 5.2: RSV1/RSV2/RSV3 MUST be 0 unless an extension
         * defines their meaning.  CWIST has no such extension. */
        if (base[0] & 0x70) return -1;

        /* Client-to-server frames must be masked per RFC 6455 section 5.3. */
        if (!(base[1] & 0x80)) return -1;

        uint64_t payload_len = base[1] & 0x7F;
        size_t hdr = 2;
        if (payload_len == 126) {
            if (ws->stash_len - off < 4) break;
            payload_len = ((uint64_t)base[2] << 8) | (uint64_t)base[3];
            hdr = 4;
        } else if (payload_len == 127) {
            if (ws->stash_len - off < 10) break;
            const uint8_t *e = base + 2;
            payload_len = ((uint64_t)e[0] << 56) | ((uint64_t)e[1] << 48) | ((uint64_t)e[2] << 40) |
                          ((uint64_t)e[3] << 32) | ((uint64_t)e[4] << 24) | ((uint64_t)e[5] << 16) |
                          ((uint64_t)e[6] << 8) | (uint64_t)e[7];
            hdr = 10;
        }

        /* Reject frames that would force a huge allocation before any
         * payload is read (OOM DoS vector). */
        if (payload_len > WS_ASYNC_MAX_PAYLOAD_BYTES) return -1;

        /* RFC 6455 section 5.5: control frames MUST NOT carry more than 125
         * bytes and MUST NOT be fragmented. */
        bool is_control_early = (opcode >= 0x8);
        if (is_control_early && (payload_len > 125 || !fin)) return -1;

        size_t total = hdr + 4 + (size_t)payload_len;
        if (ws->stash_len - off < total) break; /* Partial frame: wait for more. */

        const uint8_t *mask = base + hdr;
        const uint8_t *src = base + hdr + 4;
        uint8_t *payload = NULL;
        if (payload_len > 0) {
            payload = (uint8_t *)cwist_alloc((size_t)payload_len + 1);
            if (!payload) return -1;
            for (uint64_t i = 0; i < payload_len; i++) payload[i] = src[i] ^ mask[i % 4];
            payload[payload_len] = '\0';
        }

        off += total;
        if (ws_async_process_frame(ws, fin, opcode, payload, (size_t)payload_len) != 0) return -1;
        /* The CLOSE handler marks the connection for teardown; keep parsing
         * nothing further — the caller terminates. */
        if (ws->terminate_pending) break;
    }

    if (off > 0) {
        memmove(ws->stash, ws->stash + off, ws->stash_len - off);
        ws->stash_len -= off;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reactor integration                                                 */
/* ------------------------------------------------------------------ */

static void ws_async_read_ready(int fd, void *ctx) {
    ws_async_slot_t *slot = (ws_async_slot_t *)ctx;
    cwist_websocket_async *ws = slot->ws;
    uint8_t buf[WS_ASYNC_READ_CHUNK];
    bool eof = false, err = false;

    ws->read_armed = false;

    /* Drain everything the kernel has without blocking. */
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            if (!ws_async_stash_append(ws, (const uint8_t *)buf, (size_t)n)) {
                err = true;
                break;
            }
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        err = true;
        break;
    }

    if (!err && ws->stash_len > 0 && ws_async_feed(ws) != 0) err = true;

    /* CLOSE-from-peer, async_close() from the callback, EOF and errors all
     * converge here: tear the connection down exactly once. */
    if (err || eof || ws->terminate_pending) {
        ws_async_terminate(ws);
        return;
    }

    /* One-shot slot consumed: re-arm for the next read event. */
    if (!cwist_reactor_add(ws->reactor, fd, ws_async_read_ready, slot, sizeof(*slot))) {
        ws_async_terminate(ws);
        return;
    }
    ws->read_armed = true;
}

bool cwist_websocket_async_attach(int fd, cwist_reactor_t *reactor,
                                  cwist_ws_on_message_t on_message, void *user_data,
                                  const uint8_t *initial, size_t initial_len) {
    if (fd < 0 || !reactor || !on_message) return false;

    cwist_websocket_async *ws = (cwist_websocket_async *)cwist_alloc(sizeof(*ws));
    if (!ws) {
        close(fd);
        return false;
    }
    ws->fd = fd;
    ws->reactor = reactor;
    ws->on_message = on_message;
    ws->user_data = user_data;
    ws->stash = NULL;
    ws->stash_len = ws->stash_cap = 0;
    ws->frag_buf = NULL;
    ws->frag_len = ws->frag_cap = 0;
    ws->frag_opcode = CWIST_WS_FRAME_CONTINUATION;
    ws->park_buf = NULL;
    ws->park_off = ws->park_len = 0;
    ws->park_deadline_sec = 0;
    ws->closed = false;
    ws->terminate_pending = false;
    ws->read_armed = false;

    /* The fd must never block the reactor thread. */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    /* Bytes already read past the upgrade request (conn->rbuf) go through the
     * parser before the first reactor wait.  Delivery can happen inline here,
     * including a cwist_websocket_async_close() from the callback. */
    bool fail = false;
    if (initial_len > 0) {
        if (!ws_async_stash_append(ws, initial, initial_len)) fail = true;
        if (!fail && ws_async_feed(ws) != 0) fail = true;
    }
    if (fail || ws->terminate_pending) {
        cwist_reactor_del(reactor, fd);
        close(fd);
        cwist_free(ws->stash);
        cwist_free(ws->frag_buf);
        cwist_free(ws->park_buf);
        cwist_free(ws);
        return false;
    }

    ws_async_slot_t slot = {.ws = ws};
    if (!cwist_reactor_add(reactor, fd, ws_async_read_ready, &slot, sizeof(slot))) {
        close(fd);
        cwist_free(ws->stash);
        cwist_free(ws->frag_buf);
        cwist_free(ws->park_buf);
        cwist_free(ws);
        return false;
    }
    ws->read_armed = true;
    return true;
}
