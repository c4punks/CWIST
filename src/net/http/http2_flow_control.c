#include "cwist/net/http/http2_flow_control.h"

#include <limits.h>
#include <string.h>

/** @brief Clamp a value into a uint32 window range.
 *
 * @param value Value to clamp.
 * @param minimum Lower bound, returned when @p value is below it.
 * @param maximum Upper bound, returned when @p value is above it.
 * @return @p value truncated to uint32 when inside the range, else the
 *         nearest bound.
 */
static uint32_t cwist_http2_window_clamp(uint64_t value, uint32_t minimum, uint32_t maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return (uint32_t)value;
}

/** @brief Absolute difference between two unsigned 64-bit values.
 *
 * @param left First value.
 * @param right Second value.
 * @return `left - right` when @p left is greater, `right - left` otherwise.
 *         Safe for any input because the subtraction happens on the larger
 *         operand first.
 */
static uint64_t cwist_http2_abs_diff(uint64_t left, uint64_t right) {
    return left >= right ? left - right : right - left;
}

/** @brief Recompute network quality, target window, and pacing rate.
 *
 * No-op until the first RTT sample marks the estimator initialized.  Uses
 * srtt + 4*rttvar as the effective delay: high jitter marks the network
 * unstable, a delay above 200 ms marks it high-RTT, otherwise normal.  The
 * pacing multiplier scales with delay and gets a +1 bump when the network is
 * not normal.  The target window is clamped to
 * [min_window, max_window], and the pacing rate is derived from the
 * target window over the smoothed RTT, floored at 10x min_window so a
 * single RTT spike cannot throttle the connection.
 *
 * @param flow_control Connection flow-control state to update in place.
 */
static void cwist_http2_flow_control_adjust(cwist_http2_flow_control *flow_control) {
    uint64_t delay_us;
    uint64_t multiplier;

    if (!flow_control->rtt_initialized) {
        return;
    }

    delay_us = flow_control->srtt_us + (4U * flow_control->rttvar_us);
    if (flow_control->rttvar_us > flow_control->srtt_us / 2U) {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_UNSTABLE;
    } else if (delay_us >= 200000U) {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_HIGH_RTT;
    } else {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_NORMAL;
    }

    multiplier = delay_us / 200000U;
    if (multiplier < 1U) {
        multiplier = 1U;
    }
    if (flow_control->network_quality != CWIST_HTTP2_NETWORK_QUALITY_NORMAL) {
        ++multiplier;
    }

    flow_control->target_window = cwist_http2_window_clamp(
        (uint64_t)flow_control->min_window * multiplier,
        flow_control->min_window,
        flow_control->max_window);
    flow_control->pacing_rate_bytes_per_sec =
        ((uint64_t)flow_control->target_window * 1000000U) /
        (flow_control->srtt_us ? flow_control->srtt_us : 1U);
}

/** @brief Serialize an HTTP/2 WINDOW_UPDATE frame into a caller buffer.
 *
 * @param buffer Destination buffer, must hold at least 13 bytes.
 * @param buffer_len Available bytes in @p buffer.
 * @param written Set to 0 on entry, then to 13 on success.
 * @param stream_id Stream identifier; 0 writes a connection-level update.
 * @param increment Flow-control increment, must be non-zero per RFC 9113.
 * @return 0 on success, -1 if the arguments are invalid or the buffer is
 *         smaller than 13 bytes (nothing is written past @p buffer_len).
 */
static int cwist_http2_write_window_update(uint8_t *buffer, size_t buffer_len, size_t *written,
                                           uint32_t stream_id, uint32_t increment) {
    if (written != NULL) {
        *written = 0;
    }
    /* A WINDOW_UPDATE frame is 9 bytes of header + 4 bytes of payload;
     * accepting buffer_len < 13 used to write 4 bytes past the caller's
     * buffer. */
    if (buffer == NULL || written == NULL || buffer_len < 13U || increment == 0U) {
        return -1;
    }

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 4;
    buffer[3] = 0x08;
    buffer[4] = 0;
    buffer[5] = (uint8_t)((stream_id >> 24) & 0x7fU);
    buffer[6] = (uint8_t)(stream_id >> 16);
    buffer[7] = (uint8_t)(stream_id >> 8);
    buffer[8] = (uint8_t)stream_id;
    buffer[9] = (uint8_t)(increment >> 24);
    buffer[10] = (uint8_t)(increment >> 16);
    buffer[11] = (uint8_t)(increment >> 8);
    buffer[12] = (uint8_t)increment;
    *written = 13U;
    return 0;
}

/** @brief Initialize connection-level flow-control state.
 *
 * Windows below the protocol defaults are raised to the defaults, and the
 * maximum window is clamped to CWIST_HTTP2_MAX_WINDOW.  Zeroes the whole
 * state, then seeds receive/target/min/send windows with the initial
 * connection window, and grants half a window of initial pacing tokens at
 * 10x the initial window in bytes per second as a provisional pacing rate
 * until RTT samples recalibrate it.
 *
 * @param flow_control Connection flow-control state to initialize.
 * @param initial_connection_window Requested initial connection window;
 *        values below CWIST_HTTP2_INITIAL_CONNECTION_WINDOW select the default.
 * @param max_connection_window Upper bound for the dynamic window;
 *        raised to at least the initial window and capped at
 *        CWIST_HTTP2_MAX_WINDOW.
 */
void cwist_http2_flow_control_init(cwist_http2_flow_control *flow_control,
                                   uint32_t initial_connection_window,
                                   uint32_t max_connection_window) {
    if (flow_control == NULL) {
        return;
    }
    /* Anything below the default minimum means "use the default". */
    if (initial_connection_window < CWIST_HTTP2_INITIAL_CONNECTION_WINDOW) {
        initial_connection_window = CWIST_HTTP2_INITIAL_CONNECTION_WINDOW;
    }
    if (max_connection_window < initial_connection_window) {
        max_connection_window = initial_connection_window;
    }
    if (max_connection_window > CWIST_HTTP2_MAX_WINDOW) {
        max_connection_window = CWIST_HTTP2_MAX_WINDOW;
    }

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->receive_window = initial_connection_window;
    flow_control->target_window = initial_connection_window;
    flow_control->min_window = initial_connection_window;
    flow_control->max_window = max_connection_window;
    flow_control->send_window = initial_connection_window;
    flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_NORMAL;
    /* Start with half a window of burst credit; the bucket refills at
     * pacing_rate once RTT samples calibrate it. */
    flow_control->pacing_tokens = initial_connection_window / 2U;
    flow_control->pacing_rate_bytes_per_sec = initial_connection_window * 10U;
}

/** @brief Initialize per-stream flow-control state.
 *
 * Windows below the protocol defaults are raised to the defaults, and the
 * maximum window is clamped to CWIST_HTTP2_MAX_WINDOW.  Zeroes the whole
 * state, then records the stream identifier and seeds receive/target/min/send
 * windows with the initial stream window.
 *
 * @param flow_control Stream flow-control state to initialize.
 * @param stream_id HTTP/2 stream identifier to associate with the state.
 * @param initial_stream_window Requested initial stream window; values below
 *        CWIST_HTTP2_INITIAL_STREAM_WINDOW select the default.
 * @param max_stream_window Upper bound for the dynamic window; raised to at
 *        least the initial window and capped at CWIST_HTTP2_MAX_WINDOW.
 */
void cwist_http2_stream_flow_control_init(cwist_http2_stream_flow_control *flow_control,
                                          uint32_t stream_id, uint32_t initial_stream_window,
                                          uint32_t max_stream_window) {
    if (flow_control == NULL) {
        return;
    }
    /* Anything below the default minimum means "use the default". */
    if (initial_stream_window < CWIST_HTTP2_INITIAL_STREAM_WINDOW) {
        initial_stream_window = CWIST_HTTP2_INITIAL_STREAM_WINDOW;
    }
    if (max_stream_window < initial_stream_window) {
        max_stream_window = initial_stream_window;
    }
    if (max_stream_window > CWIST_HTTP2_MAX_WINDOW) {
        max_stream_window = CWIST_HTTP2_MAX_WINDOW;
    }

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->stream_id = stream_id;
    flow_control->receive_window = initial_stream_window;
    flow_control->target_window = initial_stream_window;
    flow_control->min_window = initial_stream_window;
    flow_control->max_window = max_stream_window;
    flow_control->send_window = initial_stream_window;
}

/** @brief Feed a new RTT sample into the smoothed estimator and retune.
 *
 * Ignores NULL state and zero samples.  The first sample seeds srtt and sets
 * rttvar to half of it; later samples feed exponential updates
 * (rttvar: 3/4 old + 1/4 deviation, srtt: 7/8 old + 1/8 sample) and then
 * trigger cwist_http2_flow_control_adjust() to recompute network quality,
 * the target window, and the pacing rate.
 *
 * @param flow_control Connection flow-control state holding the estimator.
 * @param rtt_sample_us Measured round-trip time in microseconds; must be
 *        non-zero to be accepted.
 */
void cwist_http2_flow_control_update_rtt(cwist_http2_flow_control *flow_control,
                                         uint64_t rtt_sample_us) {
    uint64_t difference;

    if (flow_control == NULL || rtt_sample_us == 0U) {
        return;
    }
    if (!flow_control->rtt_initialized) {
        flow_control->srtt_us = rtt_sample_us;
        flow_control->rttvar_us = rtt_sample_us / 2U;
        flow_control->rtt_initialized = true;
    } else {
        difference = cwist_http2_abs_diff(flow_control->srtt_us, rtt_sample_us);
        flow_control->rttvar_us = (3U * flow_control->rttvar_us + difference) / 4U;
        flow_control->srtt_us = (7U * flow_control->srtt_us + rtt_sample_us) / 8U;
    }
    cwist_http2_flow_control_adjust(flow_control);
}

/** @brief Account received DATA bytes against the connection receive window.
 *
 * @param flow_control Connection flow-control state.
 * @param bytes Number of bytes received from the peer.
 * @retval true @p bytes fit in the remaining receive window and were debited.
 * @retval false @p flow_control is NULL or @p bytes exceed the remaining
 *         window; nothing is debited.
 */
bool cwist_http2_flow_control_receive(cwist_http2_flow_control *flow_control, uint32_t bytes) {
    if (flow_control == NULL || bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

/** @brief Account received DATA bytes against a stream receive window.
 *
 * @param flow_control Stream flow-control state.
 * @param bytes Number of bytes received from the peer on the stream.
 * @retval true @p bytes fit in the remaining receive window and were debited.
 * @retval false @p flow_control is NULL or @p bytes exceed the remaining
 *         window; nothing is debited.
 */
bool cwist_http2_stream_flow_control_receive(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes) {
    if (flow_control == NULL || bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

/** @brief Record application-consumed bytes for the connection.
 *
 * Adds @p bytes to the pending WINDOW_UPDATE accumulator, saturating at
 * UINT32_MAX instead of wrapping.
 *
 * @param flow_control Connection flow-control state.
 * @param bytes Number of bytes the application consumed from the receive
 *        buffer.
 */
void cwist_http2_flow_control_consume(cwist_http2_flow_control *flow_control, uint32_t bytes) {
    if (flow_control != NULL) {
        flow_control->pending_update += bytes > UINT32_MAX - flow_control->pending_update
                                            ? UINT32_MAX - flow_control->pending_update : bytes;
    }
}

/** @brief Record application-consumed bytes for a stream.
 *
 * Adds @p bytes to the stream's pending WINDOW_UPDATE accumulator,
 * saturating at UINT32_MAX instead of wrapping.
 *
 * @param flow_control Stream flow-control state.
 * @param bytes Number of bytes the application consumed from the stream's
 *        receive buffer.
 */
void cwist_http2_stream_flow_control_consume(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes) {
    if (flow_control != NULL) {
        flow_control->pending_update += bytes > UINT32_MAX - flow_control->pending_update
                                            ? UINT32_MAX - flow_control->pending_update : bytes;
    }
}

/** @brief Retune the receive target toward 2x the measured bandwidth-delay
 * product.
 *
 * Projects the bytes consumed since the previous update over one SRTT.
 * Each retune moves at most a 2x/0.5x step away from the current target, so a
 * micro-interval jitter sample cannot slam the window between its floor and
 * ceiling (oscillation under bursty load).
 *
 * @param pending_update Bytes consumed since the previous window update.
 * @param srtt_us Smoothed round-trip time in microseconds.
 * @param interval_us Elapsed time since the previous update in microseconds.
 * @param current_target Window target currently in effect.
 * @param minimum Lower bound for the returned target.
 * @param maximum Upper bound for the returned target.
 * @return New target window, clamped to [minimum, maximum] and bounded to a
 *         2x grow / 0.5x shrink step from @p current_target; the current
 *         target when @p interval_us is zero.
 */
static uint32_t cwist_http2_flow_control_retune_target(uint32_t pending_update, uint64_t srtt_us,
                                                       uint64_t interval_us,
                                                       uint32_t current_target, uint32_t minimum,
                                                       uint32_t maximum) {
    uint64_t bdp2;
    uint64_t grow_cap;
    uint64_t shrink_floor;
    uint32_t target;

    if (interval_us == 0) {
        return minimum;
    }
    uint64_t bdp2 = (2ULL * (uint64_t)pending_update * srtt_us) / interval_us;
    return cwist_http2_window_clamp(bdp2, minimum, maximum);
}

/** @brief Compute the WINDOW_UPDATE credit to hand back to the peer.
 *
 * Tops the window up to the target, but never refunds less than what the
 * application has actually consumed.
 *
 * @param pending_update Bytes consumed by the application awaiting refund.
 * @param receive_window Current receive window.
 * @param target_window Desired steady-state window.
 * @return The larger of @p pending_update and the top-up from
 *         @p receive_window to @p target_window.
 */
static uint32_t cwist_http2_flow_control_increment(uint32_t pending_update, uint32_t receive_window,
                                                   uint32_t target_window) {
    uint32_t top_up = receive_window < target_window ? target_window - receive_window : 0U;
    return pending_update > top_up ? pending_update : top_up;
}

/** @brief Emit a connection-level WINDOW_UPDATE when enough credit is due.
 *
 * Does nothing until at least half the target window has been consumed
 * (unless @p force), then retunes the target against the measured
 * bandwidth-delay product and serializes a WINDOW_UPDATE for stream 0 into
 * the caller's buffer.  On success the receive window is credited with the
 * increment (saturating at CWIST_HTTP2_MAX_WINDOW), the pending accumulator
 * is cleared, and the update timestamp is advanced.
 *
 * @param flow_control Connection flow-control state.
 * @param buffer Destination buffer for the serialized frame.
 * @param buffer_len Available bytes in @p buffer (at least 13 required).
 * @param written Set to 0 when no frame is emitted, to 13 on success.
 * @param now_us Current time in microseconds; drives retuning.
 * @param force Emit the update even when less than half the target window
 *        is pending.
 * @return 1 when a WINDOW_UPDATE frame was written, 0 when no update is due
 *         or @p flow_control is NULL, -1 when the frame could not be
 *         serialized.
 */
int cwist_http2_flow_control_maybe_window_update(cwist_http2_flow_control *flow_control,
                                                 uint8_t *buffer, size_t buffer_len,
                                                 size_t *written, uint64_t now_us, bool force) {
    uint32_t increment;

    if (written != NULL) {
        *written = 0;
    }
    if (flow_control == NULL || flow_control->pending_update == 0U ||
        (!force && flow_control->pending_update < flow_control->target_window / 2U)) {
        return 0;
    }

    if (flow_control->rtt_initialized && flow_control->last_window_update_us != 0U &&
        now_us > flow_control->last_window_update_us) {
        flow_control->target_window = cwist_http2_flow_control_retune_target(
            flow_control->pending_update, flow_control->srtt_us,
            now_us - flow_control->last_window_update_us,
            flow_control->min_window, flow_control->max_window);
    }

    increment = cwist_http2_flow_control_increment(flow_control->pending_update,
                                                   flow_control->receive_window,
                                                   flow_control->target_window);
    if (cwist_http2_write_window_update(buffer, buffer_len, written, 0U, increment) != 0) {
        return -1;
    }
    flow_control->receive_window =
        increment > CWIST_HTTP2_MAX_WINDOW - flow_control->receive_window
            ? CWIST_HTTP2_MAX_WINDOW
            : flow_control->receive_window + increment;
    flow_control->pending_update = 0;
    flow_control->last_window_update_us = now_us;
    return 1;
}

/** @brief Emit a stream-level WINDOW_UPDATE when enough credit is due.
 *
 * Server-side only client-initiated (odd, non-zero) streams may receive a
 * stream-level WINDOW_UPDATE; other stream IDs return 0 without emitting
 * anything.  Requires at least half the target window in pending credit
 * unless @p force.  When due, the stream target is retuned against the
 * connection's RTT estimate and a WINDOW_UPDATE for the stream is
 * serialized; the stream receive window is credited (saturating at
 * CWIST_HTTP2_MAX_WINDOW), the pending accumulator is cleared, and the
 * update timestamp is advanced.
 *
 * @param connection_flow_control Connection flow-control state supplying the
 *        RTT estimate.
 * @param stream_flow_control Stream flow-control state to update.
 * @param buffer Destination buffer for the serialized frame.
 * @param buffer_len Available bytes in @p buffer (at least 13 required).
 * @param written Set to 0 when no frame is emitted, to 13 on success.
 * @param now_us Current time in microseconds; drives retuning.
 * @param force Emit the update even when less than half the target window
 *        is pending.
 * @return 1 when a WINDOW_UPDATE frame was written, 0 when the stream is
 *         ineligible or no update is due, -1 when @p flow_control arguments
 *         are NULL or the frame could not be serialized.
 */
int cwist_http2_stream_flow_control_maybe_window_update(
    cwist_http2_flow_control *connection_flow_control,
    cwist_http2_stream_flow_control *stream_flow_control, uint8_t *buffer, size_t buffer_len,
    size_t *written, uint64_t now_us, bool force) {
    uint32_t increment;

    if (written != NULL) {
        *written = 0;
    }
    if (connection_flow_control == NULL || stream_flow_control == NULL) {
        return -1;
    }
    /* Server-side only client-initiated (odd, non-zero) streams may receive
     * a stream-level WINDOW_UPDATE. */
    if (stream_flow_control->stream_id == 0U || (stream_flow_control->stream_id & 1U) == 0U) {
        return 0;
    }
    if (stream_flow_control->pending_update == 0U ||
        (!force && stream_flow_control->pending_update < stream_flow_control->target_window / 2U)) {
        return 0;
    }

    if (connection_flow_control->rtt_initialized &&
        connection_flow_control->last_window_update_us != 0U &&
        now_us > connection_flow_control->last_window_update_us) {
        stream_flow_control->target_window = cwist_http2_flow_control_retune_target(
            stream_flow_control->pending_update, connection_flow_control->srtt_us,
            now_us - connection_flow_control->last_window_update_us,
            stream_flow_control->min_window, stream_flow_control->max_window);
    }

    increment = cwist_http2_flow_control_increment(stream_flow_control->pending_update,
                                                   stream_flow_control->receive_window,
                                                   stream_flow_control->target_window);
    if (cwist_http2_write_window_update(buffer, buffer_len, written,
                                        stream_flow_control->stream_id, increment) != 0) {
        return -1;
    }
    stream_flow_control->receive_window =
        increment > CWIST_HTTP2_MAX_WINDOW - stream_flow_control->receive_window
            ? CWIST_HTTP2_MAX_WINDOW
            : stream_flow_control->receive_window + increment;
    stream_flow_control->pending_update = 0;
    stream_flow_control->last_window_update_us = now_us;
    return 1;
}

/** @brief Credit the connection send window from a WINDOW_UPDATE frame.
 *
 * Saturating add: credit is capped at CWIST_HTTP2_MAX_WINDOW.
 *
 * @param flow_control Connection flow-control state.
 * @param increment Send-window increment from the peer.
 */
void cwist_http2_flow_control_add_send_window(cwist_http2_flow_control *flow_control,
                                              uint32_t increment) {
    if (flow_control != NULL) {
        /* Saturating add: credit is capped at the protocol maximum. */
        flow_control->send_window =
            increment > CWIST_HTTP2_MAX_WINDOW - flow_control->send_window
                ? CWIST_HTTP2_MAX_WINDOW
                : flow_control->send_window + increment;
    }
}

/** @brief Credit a stream send window from a WINDOW_UPDATE frame.
 *
 * Saturating add: credit is capped at CWIST_HTTP2_MAX_WINDOW.
 *
 * @param flow_control Stream flow-control state.
 * @param increment Send-window increment from the peer.
 */
void cwist_http2_stream_flow_control_add_send_window(cwist_http2_stream_flow_control *flow_control,
                                                     uint32_t increment) {
    if (flow_control != NULL) {
        flow_control->send_window =
            increment > CWIST_HTTP2_MAX_WINDOW - flow_control->send_window
                ? CWIST_HTTP2_MAX_WINDOW
                : flow_control->send_window + increment;
    }
}

/** @brief Compute how much of a send request may proceed right now.
 *
 * Refills the pacing token bucket from the elapsed time since the last call,
 * capping the refill at one target window's worth of tokens (and capping the
 * elapsed computation so it stays inside uint64 regardless of idle time).
 * The result is the smallest of the requested size, the connection send
 * window, the stream send window, and the available pacing tokens.
 *
 * @param connection_flow_control Connection flow-control state holding the
 *        pacing bucket.
 * @param stream_flow_control Stream flow-control state holding the stream
 *        send window.
 * @param requested Number of bytes the caller wants to send.
 * @param now_us Current time in microseconds.
 * @return Allowed byte count; 0 when either flow-control pointer is NULL or
 *         no credit of any kind is available.
 */
size_t cwist_http2_flow_control_pacing_allowance(
    cwist_http2_flow_control *connection_flow_control,
    const cwist_http2_stream_flow_control *stream_flow_control, size_t requested, uint64_t now_us) {
    uint64_t elapsed;
    uint64_t added;
    size_t allowed;

    if (connection_flow_control == NULL || stream_flow_control == NULL) {
        return 0;
    }
    if (connection_flow_control->pacing_last_us == 0U) {
        connection_flow_control->pacing_last_us = now_us;
    } else if (now_us > connection_flow_control->pacing_last_us) {
        elapsed = now_us - connection_flow_control->pacing_last_us;
        added = (elapsed * connection_flow_control->pacing_rate_bytes_per_sec) / 1000000U;
        connection_flow_control->pacing_tokens += added;
        if (connection_flow_control->pacing_tokens > connection_flow_control->target_window) {
            connection_flow_control->pacing_tokens = connection_flow_control->target_window;
        }
        connection_flow_control->pacing_last_us = now_us;
    }

    allowed = requested;
    if (allowed > connection_flow_control->send_window) {
        allowed = connection_flow_control->send_window;
    }
    if (allowed > stream_flow_control->send_window) {
        allowed = stream_flow_control->send_window;
    }
    if (allowed > connection_flow_control->pacing_tokens) {
        allowed = (size_t)connection_flow_control->pacing_tokens;
    }
    return allowed;
}

/** @brief Reserve send credit for an outgoing write.
 *
 * Checks cwist_http2_flow_control_pacing_allowance() first; on success the
 * requested bytes are debited from the connection send window, the stream
 * send window, and the pacing token bucket, so the caller must actually
 * transmit the bytes it reserved.
 *
 * @param connection_flow_control Connection flow-control state.
 * @param stream_flow_control Stream flow-control state.
 * @param bytes Number of bytes to reserve.
 * @param now_us Current time in microseconds, forwarded to the allowance
 *        computation.
 * @retval true The reservation succeeded and all three credit pools were
 *         debited by @p bytes.
 * @retval false Insufficient connection/stream window or pacing tokens;
 *         nothing is debited.
 */
bool cwist_http2_flow_control_reserve_send(cwist_http2_flow_control *connection_flow_control,
                                           cwist_http2_stream_flow_control *stream_flow_control,
                                           uint32_t bytes, uint64_t now_us) {
    if (cwist_http2_flow_control_pacing_allowance(connection_flow_control, stream_flow_control,
                                                  bytes, now_us) < bytes) {
        return false;
    }
    connection_flow_control->send_window -= bytes;
    stream_flow_control->send_window -= bytes;
    connection_flow_control->pacing_tokens -= bytes;
    return true;
}
