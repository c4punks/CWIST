/* Opt-in wrk 4.2.0 post-run exporter; never called from a request worker. */
#ifndef CWIST_WRK_DUAL_HISTOGRAM_H
#define CWIST_WRK_DUAL_HISTOGRAM_H
#include <inttypes.h>
#include <stdio.h>
#include "stats.h"

static void cwist_dump_histogram(const char *kind, const stats *s) {
    printf("CWIST_HISTOGRAM {\"kind\":\"%s\",\"count\":%" PRIu64
           ",\"unit\":\"us\",\"bins\":[", kind, s->count);
    const char *separator = "";
    if (s->count) {
        /* Correction can add buckets below the original s->min. */
        for (uint64_t i = 0; i < s->limit && i <= s->max; i++) {
            if (!s->data[i]) continue;
            printf("%s[%" PRIu64 ",%" PRIu64 "]", separator, i, s->data[i]);
            separator = ",";
        }
    }
    puts("]}");
}
#endif
