/** @file sdp.c
 * @brief Minimal SDP (RFC 4566) parse/generate for DataChannel offers/answers.
 */
#include "webrtc_internal.h"

#include <stdio.h>
#include <string.h>

static void copy_attr_value(const char *line, const char *prefix, char *dst, size_t cap) {
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0)
        return;
    const char *v = line + plen;
    size_t n = strcspn(v, "\r\n");
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = '\0';
}

int cwist_sdp_parse(const char *sdp, size_t len, cwist_sdp_info *info) {
    memset(info, 0, sizeof(*info));
    char line[512];
    size_t pos = 0;
    while (pos < len) {
        size_t n = strcspn(sdp + pos, "\r\n");
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, sdp + pos, n);
        line[n] = '\0';
        pos += n;
        while (pos < len && (sdp[pos] == '\r' || sdp[pos] == '\n'))
            pos++;

        if (strncmp(line, "a=ice-ufrag:", 12) == 0)
            copy_attr_value(line, "a=ice-ufrag:", info->ice_ufrag, sizeof(info->ice_ufrag));
        else if (strncmp(line, "a=ice-pwd:", 10) == 0)
            copy_attr_value(line, "a=ice-pwd:", info->ice_pwd, sizeof(info->ice_pwd));
        else if (strncmp(line, "a=fingerprint:", 14) == 0)
            copy_attr_value(line, "a=fingerprint:sha-256 ", info->fingerprint,
                            sizeof(info->fingerprint));
        else if (strncmp(line, "a=setup:", 8) == 0)
            copy_attr_value(line, "a=setup:", info->setup, sizeof(info->setup));
        else if (strncmp(line, "a=mid:", 6) == 0)
            copy_attr_value(line, "a=mid:", info->mid, sizeof(info->mid));
        else if (strcmp(line, "a=ice-lite") == 0)
            info->has_lite = 1;
    }
    if (info->ice_ufrag[0] == '\0' || info->ice_pwd[0] == '\0')
        return -1;
    return 0;
}

static int write_session(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                         const char *pwd, const char *mid, const char *setup, int with_lite,
                         long long sess_id) {
    int n = snprintf(out, cap,
                     "v=0\r\n"
                     "o=- %lld 2 IN IP4 127.0.0.1\r\n"
                     "s=-\r\n"
                     "t=0 0\r\n"
                     "a=group:BUNDLE %s\r\n"
                     "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "a=mid:%s\r\n"
                     "a=ice-ufrag:%s\r\n"
                     "a=ice-pwd:%s\r\n"
                     "%s"
                     "a=fingerprint:sha-256 %s\r\n"
                     "a=setup:%s\r\n"
                     "a=sctp-port:5000\r\n"
                     "a=max-message-size:262144\r\n",
                     sess_id, mid, mid, ufrag, pwd, with_lite ? "a=ice-lite\r\n" : "", fingerprint,
                     setup);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int cwist_sdp_write_answer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                           const char *pwd, const char *mid, const char *host, uint16_t port) {
    /* ICE-lite answering endpoint takes the passive DTLS role. */
    int n = snprintf(out, cap,
                     "v=0\r\n"
                     "o=- 872978578 2 IN IP4 127.0.0.1\r\n"
                     "s=-\r\n"
                     "t=0 0\r\n"
                     "a=group:BUNDLE %s\r\n"
                     "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "a=mid:%s\r\n"
                     "a=ice-ufrag:%s\r\n"
                     "a=ice-pwd:%s\r\n"
                     "a=ice-lite\r\n"
                     "a=fingerprint:sha-256 %s\r\n"
                     "a=setup:passive\r\n"
                     "a=candidate:1 1 UDP 2122260223 %s %u typ host\r\n"
                     "a=end-of-candidates\r\n"
                     "a=sctp-port:5000\r\n"
                     "a=max-message-size:262144\r\n",
                     mid, mid, ufrag, pwd, fingerprint, host, (unsigned)port);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int cwist_sdp_write_offer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                          const char *pwd, const char *mid) {
    /* Full agent offering endpoint takes the active DTLS role. */
    return write_session(out, cap, fingerprint, ufrag, pwd, mid, "active", 0, 337018573);
}
