#ifndef CWIST_NUMBER_INTERNAL_H
#define CWIST_NUMBER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static inline bool cwist_number_double_is_finite(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

static inline bool cwist_number_long_double_is_finite(long double value) {
    char text[64];
    snprintf(text, sizeof(text), "%Lf", value);
    return strcasecmp(text, "nan") != 0 && strcasecmp(text, "inf") != 0 &&
           strcasecmp(text, "-inf") != 0 && strcasecmp(text, "infinity") != 0 &&
           strcasecmp(text, "-infinity") != 0;
}

/**
 * Format a finite double as the shortest decimal text that parses back to
 * the same value: integral values up to 2^53 print without an exponent,
 * everything else tries %.15g, %.16g and %.17g in turn. Plain %g keeps only
 * six significant digits and silently rounds (1234567 -> 1.23457e+06).
 */
static inline void cwist_number_format_double(char *buf, size_t len, double value) {
    if (value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
        (double)(long long)value == value) {
        snprintf(buf, len, "%lld", (long long)value);
        return;
    }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, len, "%.*g", prec, value);
        if (prec == 17 || strtod(buf, NULL) == value) return;
    }
}

#endif
