/**
 * @file test_protobuf.c
 * @brief Unit tests for the protobuf wire-format writer and reader
 * (net/grpc/protobuf.h), against byte vectors from the protobuf encoding
 * spec and against each other (write, then read back).
 */
#include <cwist/net/grpc/protobuf.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void expect_bytes(const cwist_pb_writer *w, const uint8_t *want, size_t n) {
    assert(w->len == n);
    assert(memcmp(w->data, want, n) == 0);
}

static void test_varint_vectors(void) {
    struct {
        uint64_t v;
        uint8_t bytes[10];
        size_t n;
    } cases[] = {
        {0, {0x00}, 1},
        {1, {0x01}, 1},
        {127, {0x7f}, 1},
        {128, {0x80, 0x01}, 2},
        {150, {0x96, 0x01}, 2},
        {300, {0xac, 0x02}, 2},
        {UINT32_MAX, {0xff, 0xff, 0xff, 0xff, 0x0f}, 5},
        {UINT64_MAX, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, 10},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        cwist_pb_writer w;
        cwist_pb_writer_init(&w);
        assert(cwist_pb_write_varint(&w, cases[i].v) == 0);
        expect_bytes(&w, cases[i].bytes, cases[i].n);

        cwist_pb_reader r;
        uint64_t got = 0;
        cwist_pb_reader_init(&r, w.data, w.len);
        assert(cwist_pb_read_varint(&r, &got) == 0);
        assert(got == cases[i].v && r.pos == cases[i].n);
        cwist_pb_writer_free(&w);
    }
}

static void test_varint_errors(void) {
    cwist_pb_reader r;
    uint64_t v;
    const uint8_t truncated[] = {0x96};
    cwist_pb_reader_init(&r, truncated, sizeof(truncated));
    assert(cwist_pb_read_varint(&r, &v) == -1);

    /* 11 continuation bytes: longer than any 64-bit varint. */
    uint8_t overlong[11];
    memset(overlong, 0x80, sizeof(overlong));
    cwist_pb_reader_init(&r, overlong, sizeof(overlong));
    assert(cwist_pb_read_varint(&r, &v) == -1);

    cwist_pb_reader_init(&r, truncated, 0);
    assert(cwist_pb_read_varint(&r, &v) == -1);
    assert(cwist_pb_read_varint(NULL, &v) == -1);
    assert(cwist_pb_read_varint(&r, NULL) == -1);
    assert(cwist_pb_write_varint(NULL, 1) == -1);
}

static void test_zigzag(void) {
    const int64_t s[] = {0, -1, 1, -2, 2, 2147483647, -2147483648LL, INT64_MAX, INT64_MIN};
    const uint64_t u[] = {0, 1, 2, 3, 4, 4294967294u, 4294967295u, UINT64_MAX - 1, UINT64_MAX};
    for (size_t i = 0; i < sizeof(s) / sizeof(*s); i++) {
        assert(cwist_pb_zigzag_encode(s[i]) == u[i]);
        assert(cwist_pb_zigzag_decode(u[i]) == s[i]);
    }
}

static void test_spec_messages(void) {
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);

    /* "message Test1 { int32 a = 1; }" with a = 150. */
    assert(cwist_pb_write_uint64_field(&w, 1, 150) == 0);
    expect_bytes(&w, (const uint8_t[]){0x08, 0x96, 0x01}, 3);
    w.len = 0;

    /* "message Test2 { string b = 2; }" with b = "testing". */
    assert(cwist_pb_write_string_field(&w, 2, "testing") == 0);
    expect_bytes(&w, (const uint8_t[]){0x12, 0x07, 't', 'e', 's', 't', 'i', 'n', 'g'}, 9);
    w.len = 0;

    /* Negative int64 is a 10-byte two's-complement varint. */
    assert(cwist_pb_write_int64_field(&w, 1, -1) == 0);
    expect_bytes(&w, (const uint8_t[]){0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, 11);
    w.len = 0;

    /* Fixed-width values are little-endian. */
    assert(cwist_pb_write_fixed32_field(&w, 3, 0x01020304u) == 0);
    expect_bytes(&w, (const uint8_t[]){0x1d, 0x04, 0x03, 0x02, 0x01}, 5);
    w.len = 0;
    assert(cwist_pb_write_fixed64_field(&w, 4, 0x0102030405060708ull) == 0);
    expect_bytes(&w, (const uint8_t[]){0x21, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}, 9);
    w.len = 0;
    assert(cwist_pb_write_raw32(&w, 0xaabbccddu) == 0);
    assert(cwist_pb_write_raw64(&w, 1) == 0);
    expect_bytes(&w, (const uint8_t[]){0xdd, 0xcc, 0xbb, 0xaa, 1, 0, 0, 0, 0, 0, 0, 0}, 12);
    w.len = 0;

    /* 1.0f = 0x3f800000, 1.0 = 0x3ff0000000000000. */
    assert(cwist_pb_write_float_field(&w, 5, 1.0f) == 0);
    expect_bytes(&w, (const uint8_t[]){0x2d, 0x00, 0x00, 0x80, 0x3f}, 5);
    w.len = 0;
    assert(cwist_pb_write_double_field(&w, 6, 1.0) == 0);
    expect_bytes(&w, (const uint8_t[]){0x31, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f}, 9);
    w.len = 0;

    assert(cwist_pb_write_bool_field(&w, 7, 42) == 0);
    expect_bytes(&w, (const uint8_t[]){0x38, 0x01}, 2);
    w.len = 0;

    /* Keys: field 16 needs a two-byte key. */
    assert(cwist_pb_write_key(&w, 16, CWIST_PB_LEN) == 0);
    expect_bytes(&w, (const uint8_t[]){0x82, 0x01}, 2);
    cwist_pb_writer_free(&w);
}

static void test_writer_errors_and_growth(void) {
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(w.data == NULL && w.len == 0 && w.cap == 0);
    assert(cwist_pb_write_key(&w, 0, CWIST_PB_VARINT) == -1);
    assert(cwist_pb_write_key(&w, CWIST_PB_MAX_FIELD_NUMBER + 1, CWIST_PB_VARINT) == -1);
    assert(cwist_pb_write_key(&w, CWIST_PB_MAX_FIELD_NUMBER, CWIST_PB_VARINT) == 0);
    assert(cwist_pb_write_key(&w, 1, (cwist_pb_wire_type_t)3) == -1); /* group start */
    assert(cwist_pb_write_key(&w, 1, (cwist_pb_wire_type_t)4) == -1); /* group end */
    assert(cwist_pb_write_bytes_field(&w, 1, NULL, 3) == -1);
    assert(cwist_pb_write_string_field(&w, 1, NULL) == -1);
    assert(cwist_pb_writer_reserve(NULL, 1) == -1);
    assert(cwist_pb_write_raw32(NULL, 1) == -1);
    assert(cwist_pb_write_raw64(NULL, 1) == -1);
    w.len = 0;

    /* Overflowing reserve is refused without touching the buffer. */
    assert(cwist_pb_writer_reserve(&w, 1) == 0);
    size_t cap = w.cap;
    w.len = 1;
    assert(cwist_pb_writer_reserve(&w, SIZE_MAX) == -1);
    assert(w.cap == cap);
    w.len = 0;

    /* Growth keeps earlier bytes. */
    uint8_t big[5000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    assert(cwist_pb_write_bytes_field(&w, 9, big, sizeof(big)) == 0);
    assert(cwist_pb_write_bytes_field(&w, 10, NULL, 0) == 0); /* empty bytes is valid */
    assert(w.cap >= w.len);
    cwist_pb_reader r;
    cwist_pb_field f;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(cwist_pb_read_field(&r, &f) == 1);
    assert(f.number == 9 && f.wire_type == CWIST_PB_LEN && f.len == sizeof(big));
    assert(memcmp(f.bytes, big, sizeof(big)) == 0);
    assert(cwist_pb_read_field(&r, &f) == 1);
    assert(f.number == 10 && f.len == 0);
    assert(cwist_pb_read_field(&r, &f) == 0);
    cwist_pb_writer_free(&w);
    assert(w.data == NULL && w.len == 0);
}

static void test_round_trip_all_types(void) {
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_uint64_field(&w, 1, 123456789012345ull) == 0);
    assert(cwist_pb_write_int64_field(&w, 2, -42) == 0);
    assert(cwist_pb_write_uint64_field(&w, 3, cwist_pb_zigzag_encode(-42)) == 0);
    assert(cwist_pb_write_fixed32_field(&w, 4, 7) == 0);
    assert(cwist_pb_write_fixed64_field(&w, 5, 8) == 0);
    assert(cwist_pb_write_float_field(&w, 6, -2.5f) == 0);
    assert(cwist_pb_write_double_field(&w, 7, 3.25) == 0);
    assert(cwist_pb_write_string_field(&w, 8, "hi") == 0);

    cwist_pb_reader r;
    cwist_pb_field f;
    cwist_pb_reader_init(&r, w.data, w.len);
    int seen = 0;
    int rc;
    while ((rc = cwist_pb_read_field(&r, &f)) == 1) {
        assert(cwist_pb_skip_field(&r, &f) == 0);
        seen++;
        switch (f.number) {
            case 1: assert(f.wire_type == CWIST_PB_VARINT && f.varint == 123456789012345ull); break;
            case 2: assert((int64_t)f.varint == -42); break;
            case 3: assert(cwist_pb_zigzag_decode(f.varint) == -42); break;
            case 4: assert(f.wire_type == CWIST_PB_32BIT && cwist_pb_load32_le(f.bytes) == 7); break;
            case 5: assert(f.wire_type == CWIST_PB_64BIT && cwist_pb_load64_le(f.bytes) == 8); break;
            case 6: assert(cwist_pb_load_float(f.bytes) == -2.5f); break;
            case 7: assert(cwist_pb_load_double(f.bytes) == 3.25); break;
            case 8: assert(f.len == 2 && memcmp(f.bytes, "hi", 2) == 0); break;
            default: assert(0);
        }
    }
    assert(rc == 0 && seen == 8);
    assert(cwist_pb_skip_field(&r, NULL) == -1);
    cwist_pb_writer_free(&w);
}

static void test_reader_malformed(void) {
    cwist_pb_reader r;
    cwist_pb_field f;
    struct {
        const uint8_t *b;
        size_t n;
    } bad[] = {
        {(const uint8_t[]){0x00, 0x01}, 2},                   /* field number 0 */
        {(const uint8_t[]){0x0b}, 1},                         /* wire type 3 (group) */
        {(const uint8_t[]){0x0e}, 1},                         /* wire type 6 */
        {(const uint8_t[]){0x08}, 1},                         /* varint value missing */
        {(const uint8_t[]){0x12, 0x05, 'a', 'b'}, 4},         /* LEN past the end */
        {(const uint8_t[]){0x0d, 0x01, 0x02, 0x03}, 4},       /* 32-bit short */
        {(const uint8_t[]){0x09, 1, 2, 3, 4, 5, 6, 7}, 8},    /* 64-bit short */
        {(const uint8_t[]){0x12, 0xff, 0xff, 0xff, 0xff, 0x0f}, 6}, /* huge LEN */
        /* Key (2^32 + 1) << 3: the field number does not fit in 29 bits and
         * used to be truncated to field 1. */
        {(const uint8_t[]){0x88, 0x80, 0x80, 0x80, 0x80, 0x01, 0x01}, 7},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        cwist_pb_reader_init(&r, bad[i].b, bad[i].n);
        assert(cwist_pb_read_field(&r, &f) == -1);
    }
    /* End of input and invalid arguments report "no field". */
    cwist_pb_reader_init(&r, NULL, 0);
    assert(cwist_pb_read_field(&r, &f) == 0);
    assert(cwist_pb_read_field(NULL, &f) == 0);
    assert(cwist_pb_read_field(&r, NULL) == 0);
}

int main(void) {
    test_varint_vectors();
    test_varint_errors();
    test_zigzag();
    test_spec_messages();
    test_writer_errors_and_growth();
    test_round_trip_all_types();
    test_reader_malformed();
    printf("test_protobuf: ok\n");
    return 0;
}
