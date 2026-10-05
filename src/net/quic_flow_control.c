#include <cwist/net/quic_flow_control.h>

#define QUIC_MAX_VARINT ((1ULL << 62) - 1)
#define QUIC_FRAME_MAX_DATA 0x10
#define QUIC_FRAME_MAX_STREAM_DATA 0x11

/**
 * @brief Clamp a flow control window to the protocol minimum.
 *
 * @param window Requested window size in bytes.
 * @return window if it is at least QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW,
 *         otherwise QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW.
 */
static uint64_t clamp_window(uint64_t window) {
    return window < QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW ? QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW
                                                         : window;
}

/**
 * @brief Add two values with saturation to the QUIC varint maximum.
 *
 * @param value Base value.
 * @param increment Value to add.
 * @return value + increment, or QUIC_MAX_VARINT if the sum would overflow it.
 */
static uint64_t saturating_add(uint64_t value, uint64_t increment) {
    return value > QUIC_MAX_VARINT - increment ? QUIC_MAX_VARINT : value + increment;
}

/**
 * @brief Determine the encoded size of a QUIC varint for a value.
 *
 * @param value Value to encode.
 * @return Encoded size in bytes: 1, 2, 4, or 8.
 */
static size_t varint_size(uint64_t value) {
    if (value < (1ULL << 6)) return 1;
    if (value < (1ULL << 14)) return 2;
    if (value < (1ULL << 30)) return 4;
    return 8;
}

/**
 * @brief Encode a QUIC varint into a buffer in big-endian byte order.
 *
 * Writes the value right-aligned in @p size bytes and sets the two-bit
 * length prefix in the most significant bits of the first byte.
 * The caller must ensure @p buf has at least @p size bytes available.
 *
 * @param buf Output buffer.
 * @param value Value to encode (must fit in @p size bytes).
 * @param size Encoded size: 1, 2, 4, or 8.
 */
static void write_varint(uint8_t *buf, uint64_t value, size_t size) {
    size_t i;

    for (i = size; i-- > 0; value >>= 8) buf[i] = (uint8_t)value;

    buf[0] |= (uint8_t)((size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3) << 6);
}

/**
 * @brief Check whether a MAX_DATA-style limit update should be sent.
 *
 * An update is due when the consumed amount has reached the current
 * maximum, or when at least half of the window has been consumed.
 *
 * @param consumed Total bytes consumed so far.
 * @param maximum Current advertised maximum.
 * @param window Current flow control window size.
 * @return Non-zero if an update should be sent, zero otherwise.
 */
static int should_update(uint64_t consumed, uint64_t maximum, uint64_t window) {
    return consumed >= maximum || consumed + window / 2 >= maximum;
}

/**
 * @brief Initialize the connection-level flow control state.
 *
 * Initial and maximum windows are clamped to
 * QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW, and each maximum is raised to at
 * least the corresponding initial value. No-op if @p fc is NULL.
 *
 * @param fc Flow control state to initialize.
 * @param initial_conn_window Initial connection-level window in bytes.
 * @param initial_stream_window Initial per-stream window in bytes.
 * @param max_conn_window Upper bound for the connection window in bytes.
 * @param max_stream_window Upper bound for the per-stream window in bytes.
 */
void quic_flow_control_init(quic_conn_fc_t *fc, uint64_t initial_conn_window,
                            uint64_t initial_stream_window, uint64_t max_conn_window,
                            uint64_t max_stream_window) {
    if (!fc) return;

    initial_conn_window = clamp_window(initial_conn_window);
    initial_stream_window = clamp_window(initial_stream_window);
    max_conn_window = clamp_window(max_conn_window);
    max_stream_window = clamp_window(max_stream_window);
    if (max_conn_window < initial_conn_window) max_conn_window = initial_conn_window;
    if (max_stream_window < initial_stream_window) max_stream_window = initial_stream_window;

    *fc = (quic_conn_fc_t){
        .quality = QUIC_NET_QUALITY_NORMAL,
        .max_data = initial_conn_window,
        .conn_window_size = initial_conn_window,
        .conn_min_window = initial_conn_window,
        .conn_max_window = max_conn_window,
        .base_stream_window = initial_stream_window,
        .stream_min_window = initial_stream_window,
        .stream_max_window = max_stream_window,
    };
}

/**
 * @brief Initialize the flow control state for a single stream.
 *
 * The initial window is clamped to QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW.
 * No-op if @p sfc is NULL.
 *
 * @param sfc Stream flow control state to initialize.
 * @param stream_id ID of the stream this state belongs to.
 * @param initial_stream_window Initial per-stream window in bytes.
 */
void quic_stream_fc_init(quic_stream_fc_t *sfc, uint64_t stream_id,
                         uint64_t initial_stream_window) {
    if (!sfc) return;

    initial_stream_window = clamp_window(initial_stream_window);
    *sfc = (quic_stream_fc_t){
        .stream_id = stream_id,
        .max_stream_data = initial_stream_window,
        .stream_window_size = initial_stream_window,
    };
}

/**
 * @brief Feed an RTT sample into the smoothed RTT estimator and re-evaluate quality.
 *
 * Uses RFC 6298-style smoothing: rttvar is updated with weight 1/4 and srtt
 * with weight 1/8; the first sample initializes both. Tracked state is
 * confined to @p fc and is not thread-safe across concurrent calls on the
 * same connection. No-op if @p fc is NULL or @p rtt_sample_us is zero.
 *
 * @param fc Connection flow control state.
 * @param rtt_sample_us Latest RTT sample in microseconds.
 */
void quic_flow_control_update_rtt(quic_conn_fc_t *fc, uint64_t rtt_sample_us) {
    uint64_t difference;

    if (!fc || rtt_sample_us == 0) return;

    if (!fc->rtt_initialized) {
        fc->srtt_us = rtt_sample_us;
        fc->rttvar_us = rtt_sample_us / 2;
        fc->min_rtt_us = rtt_sample_us;
        fc->rtt_initialized = true;
    } else {
        difference =
            fc->srtt_us > rtt_sample_us ? fc->srtt_us - rtt_sample_us : rtt_sample_us - fc->srtt_us;
        fc->rttvar_us = (3 * fc->rttvar_us + difference) / 4;
        fc->srtt_us = (7 * fc->srtt_us + rtt_sample_us) / 8;
        if (rtt_sample_us < fc->min_rtt_us) fc->min_rtt_us = rtt_sample_us;
    }

    quic_flow_control_evaluate_quality(fc);
}

/**
 * @brief Recompute the network quality class from the smoothed RTT.
 *
 * Classifies by srtt thresholds: <= 50 ms excellent, <= 150 ms normal,
 * <= 500 ms high RTT, above that poor. The result is stored in
 * fc->quality and returned. Returns QUIC_NET_QUALITY_NORMAL if @p fc is
 * NULL or no RTT sample has been recorded yet.
 *
 * @param fc Connection flow control state.
 * @return The (updated) quality classification.
 */
quic_net_quality_t quic_flow_control_evaluate_quality(quic_conn_fc_t *fc) {
    if (!fc || !fc->rtt_initialized) return QUIC_NET_QUALITY_NORMAL;

    if (fc->srtt_us <= 50000)
        fc->quality = QUIC_NET_QUALITY_EXCELLENT;
    else if (fc->srtt_us <= 150000)
        fc->quality = QUIC_NET_QUALITY_NORMAL;
    else if (fc->srtt_us <= 500000)
        fc->quality = QUIC_NET_QUALITY_HIGH_RTT;
    else
        fc->quality = QUIC_NET_QUALITY_POOR;

    return fc->quality;
}

/**
 * @brief Grow flow control windows when the network quality is degraded.
 *
 * Only acts when the connection quality is at least QUIC_NET_QUALITY_HIGH_RTT.
 * Each window that is below its configured maximum is doubled, capped at
 * that maximum. No-op if @p fc is NULL or quality is better than
 * QUIC_NET_QUALITY_HIGH_RTT; @p sfc may be NULL, in which case only the
 * connection window is adjusted.
 *
 * @param fc Connection flow control state.
 * @param sfc Stream flow control state to adjust, or NULL.
 */
void quic_flow_control_adjust_windows(quic_conn_fc_t *fc, quic_stream_fc_t *sfc) {
    if (!fc || fc->quality < QUIC_NET_QUALITY_HIGH_RTT) return;

    if (fc->conn_window_size < fc->conn_max_window) {
        fc->conn_window_size *= 2;
        if (fc->conn_window_size > fc->conn_max_window) fc->conn_window_size = fc->conn_max_window;
    }

    if (sfc && sfc->stream_window_size < fc->stream_max_window) {
        sfc->stream_window_size *= 2;
        if (sfc->stream_window_size > fc->stream_max_window)
            sfc->stream_window_size = fc->stream_max_window;
    }
}

/**
 * @brief Account for connection-level flow control bytes consumed by the peer.
 *
 * The consumed counter saturates at the QUIC varint maximum instead of
 * wrapping. No-op if @p fc is NULL.
 *
 * @param fc Connection flow control state.
 * @param bytes Number of bytes consumed.
 */
void quic_flow_control_consume_conn(quic_conn_fc_t *fc, uint64_t bytes) {
    if (fc) fc->conn_consumed = saturating_add(fc->conn_consumed, bytes);
}

/**
 * @brief Account for stream-level flow control bytes consumed by the peer.
 *
 * The consumed counter saturates at the QUIC varint maximum instead of
 * wrapping. The connection state @p fc is not touched; @p sfc may be NULL,
 * in which case the call is a no-op.
 *
 * @param fc Connection flow control state (currently unused).
 * @param sfc Stream flow control state.
 * @param bytes Number of bytes consumed.
 */
void quic_flow_control_consume_stream(quic_conn_fc_t *fc, quic_stream_fc_t *sfc, uint64_t bytes) {
    (void)fc;
    if (sfc) sfc->stream_consumed = saturating_add(sfc->stream_consumed, bytes);
}

/**
 * @brief Write a MAX_DATA frame if the connection limit should be raised.
 *
 * Unless @p force is set, a frame is produced only when at least half of
 * the connection window has been consumed (see should_update()). The new
 * maximum is consumed + window, never lower than the current maximum, and
 * is stored back in fc->max_data on success. On entry *written is cleared
 * to 0 when @p written is non-NULL.
 *
 * @param fc Connection flow control state.
 * @param buf Output buffer for the encoded frame.
 * @param buf_len Capacity of @p buf in bytes.
 * @param written Receives the encoded frame length in bytes.
 * @param force Emit the frame even if the update threshold is not met.
 * @retval 1 Frame written; *written holds its length.
 * @retval 0 No update needed; nothing written.
 * @retval -1 Invalid argument or @p buf too small; nothing written.
 */
int quic_flow_control_maybe_send_max_data(quic_conn_fc_t *fc, uint8_t *buf, size_t buf_len,
                                          size_t *written, bool force) {
    uint64_t maximum;
    size_t value_size;

    if (written) *written = 0;
    if (!fc || !buf || !written) return -1;
    if (!force && !should_update(fc->conn_consumed, fc->max_data, fc->conn_window_size)) return 0;

    maximum = saturating_add(fc->conn_consumed, fc->conn_window_size);
    if (maximum < fc->max_data) maximum = fc->max_data;
    value_size = varint_size(maximum);
    if (buf_len < 1 + value_size) return -1;

    buf[0] = QUIC_FRAME_MAX_DATA;
    write_varint(buf + 1, maximum, value_size);
    fc->max_data = maximum;
    *written = 1 + value_size;
    return 1;
}

/**
 * @brief Write a MAX_STREAM_DATA frame if the stream limit should be raised.
 *
 * Unless @p force is set, a frame is produced only when at least half of
 * the stream window has been consumed (see should_update()). The new
 * maximum is consumed + window, never lower than the current maximum, and
 * is stored back in sfc->max_stream_data on success. On entry *written is
 * cleared to 0 when @p written is non-NULL.
 *
 * @param fc Connection flow control state.
 * @param sfc Stream flow control state.
 * @param buf Output buffer for the encoded frame.
 * @param buf_len Capacity of @p buf in bytes.
 * @param written Receives the encoded frame length in bytes.
 * @param force Emit the frame even if the update threshold is not met.
 * @retval 1 Frame written; *written holds its length.
 * @retval 0 No update needed; nothing written.
 * @retval -1 Invalid argument or @p buf too small; nothing written.
 */
int quic_flow_control_maybe_send_max_stream_data(quic_conn_fc_t *fc, quic_stream_fc_t *sfc,
                                                 uint8_t *buf, size_t buf_len, size_t *written,
                                                 bool force) {
    uint64_t maximum;
    size_t id_size;
    size_t value_size;

    if (written) *written = 0;
    if (!fc || !sfc || !buf || !written) return -1;
    if (!force &&
        !should_update(sfc->stream_consumed, sfc->max_stream_data, sfc->stream_window_size))
        return 0;

    maximum = saturating_add(sfc->stream_consumed, sfc->stream_window_size);
    if (maximum < sfc->max_stream_data) maximum = sfc->max_stream_data;
    id_size = varint_size(sfc->stream_id);
    value_size = varint_size(maximum);
    if (buf_len < 1 + id_size + value_size) return -1;

    buf[0] = QUIC_FRAME_MAX_STREAM_DATA;
    write_varint(buf + 1, sfc->stream_id, id_size);
    write_varint(buf + 1 + id_size, maximum, value_size);
    sfc->max_stream_data = maximum;
    *written = 1 + id_size + value_size;
    return 1;
}
