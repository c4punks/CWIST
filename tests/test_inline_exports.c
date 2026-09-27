/**
 * @file test_inline_exports.c
 * @brief Out-of-line wrappers for public static inline helpers.
 *
 * Each *_extern() wrapper must return exactly what its inline helper returns
 * for every input, and must be a real linkable symbol: this file calls them
 * through function pointers, the way a binding generated from the headers
 * does, so it only links if libcwist.a defines them. The Makefile rule also
 * checks the symbols with nm.
 */
#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                        \
        }                                                                   \
    } while (0)

/* Held through pointers so the calls cannot be resolved inline. */
static bool (*const is_ok_extern)(const cwist_error_t *) = cwist_error_is_ok_extern;
static bool (*const endpoint_has_extern)(cwist_endpoint_opt_t,
                                         cwist_endpoint_opt_t) = cwist_endpoint_has_extern;

static int g_error_cases = 0;

static void same_is_ok(const cwist_error_t *err, bool expected) {
    REQUIRE(cwist_error_is_ok(err) == expected);
    REQUIRE(is_ok_extern(err) == expected);
    g_error_cases++;
}

static void test_error_is_ok(void) {
    same_is_ok(NULL, true);

    /* Every integer channel: zero is success, non-zero is failure. */
    static const struct {
        cwist_errtype_t type;
    } channels[] = {{CWIST_ERR_INT8},  {CWIST_ERR_INT16},  {CWIST_ERR_INT32},  {CWIST_ERR_INT64},
                    {CWIST_ERR_UINT8}, {CWIST_ERR_UINT16}, {CWIST_ERR_UINT32}, {CWIST_ERR_UINT64}};
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        cwist_error_t err = make_error(channels[i].type);
        same_is_ok(&err, true);
        switch (channels[i].type) {
            case CWIST_ERR_INT8: err.error.err_i8 = -1; break;
            case CWIST_ERR_INT16: err.error.err_i16 = -1; break;
            case CWIST_ERR_INT32: err.error.err_i32 = -1; break;
            case CWIST_ERR_INT64: err.error.err_i64 = -1; break;
            case CWIST_ERR_UINT8: err.error.err_u8 = 1; break;
            case CWIST_ERR_UINT16: err.error.err_u16 = 1; break;
            case CWIST_ERR_UINT32: err.error.err_u32 = 1; break;
            case CWIST_ERR_UINT64: err.error.err_u64 = 1; break;
            default: REQUIRE(0);
        }
        same_is_ok(&err, false);
    }

    /* Pointer channels: NULL is success, a payload is failure. */
    cwist_error_t json = make_error(CWIST_ERR_JSON);
    same_is_ok(&json, true);
    json.error.err_json = cJSON_CreateObject();
    REQUIRE(json.error.err_json != NULL);
    same_is_ok(&json, false);
    cwist_error_dispose(&json);
    same_is_ok(&json, true);

    cwist_error_t str = make_error(CWIST_ERR_STRING);
    same_is_ok(&str, true);
    cwist_sstring *msg = cwist_sstring_create();
    REQUIRE(msg != NULL);
    str.error.err_string = msg;
    same_is_ok(&str, false);
    cwist_sstring_destroy(msg);

    /* An errtype outside the enum is never success. */
    cwist_error_t bogus = make_error(CWIST_ERR_INT16);
    bogus.errtype = (cwist_errtype_t)0x7f;
    same_is_ok(&bogus, false);

    printf("Passed cwist_error_is_ok_extern matches the inline helper (%d cases)\n", g_error_cases);
}

static void test_endpoint_has(void) {
    int cases = 0;
    /* Every combination of the defined flags plus an undefined high bit. */
    const cwist_endpoint_opt_t bits[] = {0, CWIST_DYNAMIC, CWIST_ENDPOINT_FIXED,
                                         CWIST_ENDPOINT_FILE, (cwist_endpoint_opt_t)1u << 31};
    const size_t n = sizeof(bits) / sizeof(bits[0]);
    for (size_t a = 0; a < (1u << n); a++) {
        cwist_endpoint_opt_t opts = 0;
        for (size_t i = 0; i < n; i++) {
            if (a & (1u << i)) opts |= bits[i];
        }
        for (size_t f = 0; f < n; f++) {
            for (size_t g = 0; g < n; g++) {
                cwist_endpoint_opt_t flag = bits[f] | bits[g];
                bool expected = (opts & flag) != 0;
                REQUIRE(cwist_endpoint_has(opts, flag) == expected);
                REQUIRE(endpoint_has_extern(opts, flag) == expected);
                cases++;
            }
        }
    }
    REQUIRE(endpoint_has_extern(CWIST_ENDPOINT_DEFAULT, CWIST_ENDPOINT_DEFAULT) ==
            (CWIST_ENDPOINT_DEFAULT != 0));
    printf("Passed cwist_endpoint_has_extern matches the inline helper (%d cases)\n", cases);
}

int main(void) {
    printf("Testing exported wrappers for static inline helpers...\n");
    test_error_is_ok();
    test_endpoint_has();
    printf("All inline export tests passed!\n");
    return 0;
}
