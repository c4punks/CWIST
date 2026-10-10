/**
 * @file protobuf.c
 * @brief Small Protobuf wire-format reader/writer helpers.
 */

#include <cwist/net/grpc/protobuf.h>
#include <cwist/core/mem/alloc.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initialize a Protobuf writer to an empty buffer.
 * @param w Writer to reset. NULL is accepted and ignored.
 */
void cwist_pb_writer_init(cwist_pb_writer *w) {
    if (!w) return;
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

/**
 * @brief Release the buffer owned by a Protobuf writer.
 * @param w Writer whose data is freed and reset. NULL is accepted and ignored.
 */
void cwist_pb_writer_free(cwist_pb_writer *w) {
    if (!w) return;
    cwist_free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

/**
 * @brief Ensure the writer buffer has room for @p extra more bytes.
 * @param w Writer to grow. Must not be NULL.
 * @param extra Number of additional bytes required beyond the current length.
 * @return 0 on success, -1 on invalid argument, overflow, or allocation failure.
 */
int cwist_pb_writer_reserve(cwist_pb_writer *w, size_t extra) {
    if (!w) return -1;
    if (extra > SIZE_MAX - w->len) return -1;
    size_t need = w->len + extra;
    if (need <= w->cap) return 0;

    size_t cap = w->cap ? w->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    uint8_t *next = (uint8_t *)cwist_realloc(w->data, cap);
    if (!next) return -1;
    w->data = next;
    w->cap = cap;
    return 0;
}

/**
 * @brief Append a base-128 varint to the writer buffer.
 * @param w Writer to append to. Must not be NULL.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_varint(cwist_pb_writer *w, uint64_t value) {
    if (!w) return -1;
    if (cwist_pb_writer_reserve(w, 10) != 0) return -1;
    while (value >= 0x80) {
        w->data[w->len++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    w->data[w->len++] = (uint8_t)value;
    return 0;
}

/**
 * @brief Write a field key (field number and wire type) as a varint.
 * @param w Writer to append to. Must not be NULL.
 * @param field_number Protobuf field number; must be non-zero.
 * @param wire_type Wire type; must be a valid cwist_pb_wire_type_t value.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_key(cwist_pb_writer *w, uint32_t field_number, cwist_pb_wire_type_t wire_type) {
    if (!w || field_number == 0) return -1;
    if (wire_type != CWIST_PB_VARINT && wire_type != CWIST_PB_64BIT && wire_type != CWIST_PB_LEN &&
        wire_type != CWIST_PB_32BIT) {
        return -1;
    }
    uint64_t key = ((uint64_t)field_number << 3) | (uint64_t)wire_type;
    return cwist_pb_write_varint(w, key);
}

/**
 * @brief Write a uint64 field as a varint.
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_uint64_field(cwist_pb_writer *w, uint32_t field_number, uint64_t value) {
    if (cwist_pb_write_key(w, field_number, CWIST_PB_VARINT) != 0) return -1;
    return cwist_pb_write_varint(w, value);
}

/**
 * @brief Write an int64 field; negative values are encoded as ten-byte varints.
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode (two's-complement, reinterpreted as uint64).
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_int64_field(cwist_pb_writer *w, uint32_t field_number, int64_t value) {
    return cwist_pb_write_uint64_field(w, field_number, (uint64_t)value);
}

/**
 * @brief Write a bool field as a 0/1 varint.
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Non-zero encodes as 1, zero encodes as 0.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_bool_field(cwist_pb_writer *w, uint32_t field_number, int value) {
    return cwist_pb_write_uint64_field(w, field_number, value ? 1 : 0);
}

/**
 * @brief Append a 32-bit value in little-endian order (no key written).
 * @param w Writer to append to. Must not be NULL.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_raw32(cwist_pb_writer *w, uint32_t value) {
    if (!w) return -1;
    if (cwist_pb_writer_reserve(w, 4) != 0) return -1;
    w->data[w->len++] = (uint8_t)value;
    w->data[w->len++] = (uint8_t)(value >> 8);
    w->data[w->len++] = (uint8_t)(value >> 16);
    w->data[w->len++] = (uint8_t)(value >> 24);
    return 0;
}

/**
 * @brief Append a 64-bit value in little-endian order (no key written).
 * @param w Writer to append to. Must not be NULL.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_raw64(cwist_pb_writer *w, uint64_t value) {
    if (!w) return -1;
    if (cwist_pb_writer_reserve(w, 8) != 0) return -1;
    for (unsigned i = 0; i < 8; i++) w->data[w->len++] = (uint8_t)(value >> (i * 8));
    return 0;
}

/**
 * @brief Write a fixed32 field (key plus 4 little-endian bytes).
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_fixed32_field(cwist_pb_writer *w, uint32_t field_number, uint32_t value) {
    if (cwist_pb_write_key(w, field_number, CWIST_PB_32BIT) != 0) return -1;
    return cwist_pb_write_raw32(w, value);
}

/**
 * @brief Write a fixed64 field (key plus 8 little-endian bytes).
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_fixed64_field(cwist_pb_writer *w, uint32_t field_number, uint64_t value) {
    if (cwist_pb_write_key(w, field_number, CWIST_PB_64BIT) != 0) return -1;
    return cwist_pb_write_raw64(w, value);
}

/**
 * @brief Write a float field by reinterpreting it as fixed32 bits.
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_float_field(cwist_pb_writer *w, uint32_t field_number, float value) {
    return cwist_pb_write_fixed32_field(w, field_number, cwist_pb_float_bits(value));
}

/**
 * @brief Write a double field by reinterpreting it as fixed64 bits.
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value Value to encode.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_double_field(cwist_pb_writer *w, uint32_t field_number, double value) {
    return cwist_pb_write_fixed64_field(w, field_number, cwist_pb_double_bits(value));
}

/**
 * @brief Write a length-delimited bytes field (key, length varint, raw bytes).
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param data Bytes to copy; may be NULL only when @p len is 0. The bytes are
 *             copied into the writer buffer, so the caller keeps ownership.
 * @param len Number of bytes to write.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_bytes_field(cwist_pb_writer *w, uint32_t field_number, const void *data,
                               size_t len) {
    if (!w || (len > 0 && !data)) return -1;
    if (cwist_pb_write_key(w, field_number, CWIST_PB_LEN) != 0) return -1;
    if (cwist_pb_write_varint(w, (uint64_t)len) != 0) return -1;
    if (cwist_pb_writer_reserve(w, len) != 0) return -1;
    if (len > 0) memcpy(w->data + w->len, data, len);
    w->len += len;
    return 0;
}

/**
 * @brief Write a string field (length-delimited, excluding the NUL terminator).
 * @param w Writer to append to.
 * @param field_number Protobuf field number; must be non-zero.
 * @param value NUL-terminated string to copy; must not be NULL.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int cwist_pb_write_string_field(cwist_pb_writer *w, uint32_t field_number, const char *value) {
    if (!value) return -1;
    return cwist_pb_write_bytes_field(w, field_number, value, strlen(value));
}

/**
 * @brief Initialize a reader over a caller-owned buffer.
 * @param r Reader to initialize. NULL is accepted and ignored.
 * @param data Buffer to read from; not copied, so it must outlive the reader.
 * @param len Size of @p data in bytes.
 */
void cwist_pb_reader_init(cwist_pb_reader *r, const void *data, size_t len) {
    if (!r) return;
    r->data = (const uint8_t *)data;
    r->len = len;
    r->pos = 0;
}

/**
 * @brief Read a base-128 varint, advancing the reader position.
 * @param r Reader to read from. Must not be NULL.
 * @param out Receives the decoded value. Must not be NULL.
 * @return 0 on success, -1 if the reader is exhausted or the varint is
 *         malformed (unterminated or longer than 64 bits).
 */
int cwist_pb_read_varint(cwist_pb_reader *r, uint64_t *out) {
    if (!r || !out) return -1;
    uint64_t value = 0;
    unsigned shift = 0;
    while (r->pos < r->len && shift <= 63) {
        uint8_t byte = r->data[r->pos++];
        value |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) {
            *out = value;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

/**
 * @brief Read the next field key and payload from the reader.
 * @param r Reader to read from. Must not be NULL.
 * @param field Receives the field number, wire type, and payload descriptor.
 *              Must not be NULL.
 * @retval 1 A field was read and @p field is filled in.
 * @retval 0 The reader is exhausted (end of input); the reader position is
 *            unchanged in this case.
 * @retval -1 Malformed data or invalid argument.
 */
int cwist_pb_read_field(cwist_pb_reader *r, cwist_pb_field *field) {
    if (!r || !field || r->pos >= r->len) return 0;
    memset(field, 0, sizeof(*field));

    uint64_t key = 0;
    if (cwist_pb_read_varint(r, &key) != 0) return -1;
    field->number = (uint32_t)(key >> 3);
    field->wire_type = (cwist_pb_wire_type_t)(key & 0x07);
    if (field->number == 0) return -1;

    switch (field->wire_type) {
        case CWIST_PB_VARINT:
            if (cwist_pb_read_varint(r, &field->varint) != 0) return -1;
            return 1;
        case CWIST_PB_LEN: {
            uint64_t len = 0;
            if (cwist_pb_read_varint(r, &len) != 0) return -1;
            if (len > SIZE_MAX || (size_t)len > r->len - r->pos) return -1;
            field->bytes = r->data + r->pos;
            field->len = (size_t)len;
            r->pos += field->len;
            return 1;
        }
        case CWIST_PB_32BIT:
            if (r->len - r->pos < 4) return -1;
            field->bytes = r->data + r->pos;
            field->len = 4;
            r->pos += 4;
            return 1;
        case CWIST_PB_64BIT:
            if (r->len - r->pos < 8) return -1;
            field->bytes = r->data + r->pos;
            field->len = 8;
            r->pos += 8;
            return 1;
        default: return -1;
    }
}

/**
 * @brief Validate a parsed field descriptor.
 * @note The reader position is already advanced by cwist_pb_read_field(), so
 *       this function only checks that @p field is non-NULL.
 * @param r Reader associated with the field; unused.
 * @param field Field descriptor to validate. Must not be NULL.
 * @return 0 if @p field is valid, -1 if it is NULL.
 */
int cwist_pb_skip_field(cwist_pb_reader *r, const cwist_pb_field *field) {
    (void)r;
    return field ? 0 : -1;
}

/**
 * @brief ZigZag-encode a signed 64-bit value for sint fields.
 * @param value Signed value to encode.
 * @return The ZigZag-encoded unsigned value: small magnitudes map to small
 *         unsigned codes regardless of sign (0, -1, 1, -2, 2 -> 0, 1, 2, 3, 4).
 */
uint64_t cwist_pb_zigzag_encode(int64_t value) {
    /* All ones for a negative value, zero otherwise, without the
     * implementation-defined right shift of a negative signed value. */
    return ((uint64_t)value << 1) ^ ((uint64_t)0 - ((uint64_t)value >> 63));
}

/**
 * @brief ZigZag-decode an unsigned 64-bit value back to a signed value.
 * @param value ZigZag-encoded value, as produced by cwist_pb_zigzag_encode().
 * @return The original signed value.
 */
int64_t cwist_pb_zigzag_decode(uint64_t value) {
    return (int64_t)((value >> 1) ^ (uint64_t)-((int64_t)value & 1));
}
