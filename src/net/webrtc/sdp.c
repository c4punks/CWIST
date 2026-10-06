/** @file sdp.c
 * @brief Minimal SDP (RFC 4566) parse/generate for DataChannel offers/answers.
 *
 * Supports only the subset of SDP needed for a single SCTP DataChannel
 * m-section: ICE credentials, DTLS fingerprint/setup, mid, and ice-lite.
 * Parsing extracts fields into a cwist_sdp_info struct; generation produces
 * offer or answer strings with CRLF line endings.
 */
#include "webrtc_internal.h"

#include <stdio.h>
#include <string.h>

/** @brief Copy the value part of an attribute line after a fixed prefix.
 * @param line   NUL-terminated SDP line beginning with @p prefix.
 * @param prefix Attribute prefix (e.g. "a=ice-ufrag:") to strip.
 * @param dst    Destination buffer, always NUL-terminated on success.
 * @param cap    Capacity of @p dst in bytes.
 * @note Does nothing if @p line does not start with @p prefix.
 *       Silently truncates values longer than cap - 1.
 */
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

/** @brief Parse an SDP blob into a cwist_sdp_info structure.
 * @param sdp   SDP text (not required to be NUL-terminated).
 * @param len   Length of @p sdp in bytes.
 * @param info  Output structure; zeroed first, then filled from recognized lines.
 * @return 0 on success, -1 if ice-ufrag or ice-pwd is missing or empty.
 * @note Lines longer than 511 bytes are truncated. If an attribute appears
 *       more than once, the last occurrence wins. Sets info->has_lite when
 *       "a=ice-lite" appears.
 */
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

/** @brief Emit the shared session/m-section portion of an SDP offer.
 * @param out         Output buffer, receives CRLF-terminated SDP text.
 * @param cap         Capacity of @p out in bytes.
 * @param fingerprint DTLS certificate fingerprint (hex, colon-separated).
 * @param ufrag       ICE username fragment.
 * @param pwd         ICE password.
 * @param mid         Media section ID for the BUNDLE group and m-line.
 * @param setup       DTLS setup attribute value ("active"/"passive").
 * @param with_lite   Nonzero to include an "a=ice-lite" attribute line.
 * @param sess_id     o= line session ID.
 * @return 0 on success, -1 if the output was truncated or encoding failed.
 * @warning Not NUL-guaranteed on truncation; failure must be checked before use.
 */
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

/** @brief Generate an SDP answer for an ICE-lite DataChannel endpoint.
 * @param out         Output buffer, receives CRLF-terminated SDP answer.
 * @param cap         Capacity of @p out in bytes.
 * @param fingerprint DTLS certificate fingerprint (hex, colon-separated).
 * @param ufrag       ICE username fragment.
 * @param pwd         ICE password.
 * @param mid         Media section ID (also used in the BUNDLE group).
 * @param host        Local host address for the a=candidate line.
 * @param port        Local UDP port for the a=candidate line.
 * @return 0 on success, -1 if the output was truncated or encoding failed.
 * @note Declares ice-lite and hardcodes a=setup:passive, since the
 *       ICE-lite answering endpoint takes the passive DTLS role, and emits
 *       a single host candidate followed by end-of-candidates.
 */
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

/** @brief Generate an SDP offer for a full (non-lite) DataChannel endpoint.
 * @param out         Output buffer, receives CRLF-terminated SDP offer.
 * @param cap         Capacity of @p out in bytes.
 * @param fingerprint DTLS certificate fingerprint (hex, colon-separated).
 * @param ufrag       ICE username fragment.
 * @param pwd         ICE password.
 * @param mid         Media section ID (also used in the BUNDLE group).
 * @return 0 on success, -1 if the output was truncated or encoding failed.
 * @note Thin wrapper over write_session() with setup "active", no ice-lite,
 *       and a fixed session ID; the full agent offering endpoint takes the
 *       active DTLS role.
 */
int cwist_sdp_write_offer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                          const char *pwd, const char *mid) {
    /* Full agent offering endpoint takes the active DTLS role. */
    return write_session(out, cap, fingerprint, ufrag, pwd, mid, "active", 0, 337018573);
}
