#ifndef CWIST_NUMBER_INTERNAL_H
#define CWIST_NUMBER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

#endif
