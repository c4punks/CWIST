#ifndef CWIST_JSON_INTERNAL_H
#define CWIST_JSON_INTERNAL_H

#include <stdbool.h>

static bool cwist_json_has_nul_escape(const char *json) {
    bool in_string = false;
    for (const char *p = json; *p; p++) {
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string || *p != '\\') continue;
        if (p[1] == 'u' && p[2] == '0' && p[3] == '0' && p[4] == '0' && p[5] == '0') return true;
        if (p[1]) p++;
    }
    return false;
}

#endif
