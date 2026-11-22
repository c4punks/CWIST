/** @file sdp.c
 * @brief Minimal SDP (RFC 4566) parse/generate for DataChannel offers/answers.
 *
 * Supports the SDP subset needed for one accepted SCTP DataChannel m-section:
 * ICE credentials, DTLS fingerprint/setup, mid, and ice-lite. Other offered
 * m-sections are preserved as rejected answers with port zero.
 * Parsing extracts fields into a cwist_sdp_info struct; generation produces
 * offer or answer strings with CRLF line endings.
 */
#include "webrtc_internal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <openssl/rand.h>

/** @brief Copy a recognized attribute value without accepting truncation. */
static int copy_attr_value(const char *line, const char *prefix, char *dst, size_t cap) {
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0) return -1;
    const char *v = line + plen;
    size_t n = strcspn(v, "\r\n");
    if (n >= cap) return -1;
    memcpy(dst, v, n);
    dst[n] = '\0';
    return 0;
}

/** @brief Parse an SDP blob into a cwist_sdp_info structure.
 * @param sdp   SDP text (not required to be NUL-terminated).
 * @param len   Length of @p sdp in bytes.
 * @param info  Output structure; zeroed first, then filled from recognized lines.
 * @return 0 on success, -1 if a recognized value is over-limit or required ICE
 *         credentials are missing or empty.
 * @note If an attribute appears more than once, the last occurrence wins.
 *       Sets info->has_lite when "a=ice-lite" appears.
 */
int cwist_sdp_parse(const char *sdp, size_t len, cwist_sdp_info *info) {
    if (!sdp || !info) return -1;
    memset(info, 0, sizeof(*info));
    char line[512];
    size_t pos = 0;
    int current = -1;
    while (pos < len) {
        size_t line_len = 0;
        while (pos + line_len < len && sdp[pos + line_len] != '\r' && sdp[pos + line_len] != '\n')
            line_len++;
        if (line_len >= sizeof(line)) return -1;
        memcpy(line, sdp + pos, line_len);
        line[line_len] = '\0';
        pos += line_len;
        while (pos < len && (sdp[pos] == '\r' || sdp[pos] == '\n')) pos++;

        if (strncmp(line, "m=", 2) == 0) {
            if (info->section_count == CWIST_SDP_MAX_SECTIONS) return -1;
            current = (int)info->section_count++;
            cwist_sdp_section *section = &info->sections[current];
            if (sscanf(line, "m=%15s %*s %31s %31s", section->media, section->proto,
                       section->fmt) != 3)
                return -1;
            section->is_data = strcmp(section->media, "application") == 0 &&
                               strcmp(section->proto, "UDP/DTLS/SCTP") == 0 &&
                               strcmp(section->fmt, "webrtc-datachannel") == 0;
        } else if (strncmp(line, "a=ice-ufrag:", 12) == 0) {
            if (copy_attr_value(line, "a=ice-ufrag:", info->ice_ufrag, sizeof(info->ice_ufrag)) < 0)
                return -1;
        } else if (strncmp(line, "a=ice-pwd:", 10) == 0) {
            if (copy_attr_value(line, "a=ice-pwd:", info->ice_pwd, sizeof(info->ice_pwd)) < 0)
                return -1;
        } else if (strncmp(line, "a=fingerprint:", 14) == 0) {
            const char *value = line + 14;
            while (*value == ' ' || *value == '\t') value++;
            const char *algorithm_end = strchr(value, ' ');
            if (!algorithm_end) return -1;
            if ((size_t)(algorithm_end - value) == 7 && strncasecmp(value, "sha-256", 7) == 0) {
                value = algorithm_end + 1;
                while (*value == ' ' || *value == '\t') value++;
                if (copy_attr_value(value, "", info->fingerprint, sizeof(info->fingerprint)) < 0)
                    return -1;
            }
        } else if (strncmp(line, "a=setup:", 8) == 0) {
            if (copy_attr_value(line, "a=setup:", info->setup, sizeof(info->setup)) < 0) return -1;
        } else if (strncmp(line, "a=mid:", 6) == 0) {
            char mid[16];
            if (copy_attr_value(line, "a=mid:", mid, sizeof(mid)) < 0) return -1;
            if (current >= 0) {
                cwist_sdp_section *section = &info->sections[current];
                snprintf(section->mid, sizeof(section->mid), "%s", mid);
                if (section->is_data && info->mid[0] == '\0')
                    snprintf(info->mid, sizeof(info->mid), "%s", mid);
            }
        } else if (strcmp(line, "a=ice-lite") == 0) {
            info->has_lite = 1;
        }
    }
    if (info->ice_ufrag[0] == '\0' || info->ice_pwd[0] == '\0') return -1;
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
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

/** @brief Append formatted SDP text without exceeding the output buffer. */
static int sdp_append(char *out, size_t cap, size_t *offset, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *offset, cap - *offset, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *offset) return -1;
    *offset += (size_t)n;
    return 0;
}

int cwist_sdp_write_answer_for_offer(char *out, size_t cap, const char *fingerprint,
                                     const char *ufrag, const char *pwd,
                                     const cwist_sdp_info *offer, const char *host, uint16_t port) {
    if (!out || !offer || !host || offer->section_count == 0) return -1;
    unsigned long long session_id;
    if (RAND_bytes((unsigned char *)&session_id, sizeof(session_id)) != 1) return -1;

    /* Only the accepted data section is bundled: an m-section rejected in
     * the answer (port 0) must not be in its BUNDLE group (RFC 8843 section
     * 7.3.3), and libwebrtc refuses such an answer (#993). */
    char bundle[CWIST_SDP_MAX_SECTIONS * 20];
    bundle[0] = '\0';
    for (size_t i = 0; i < offer->section_count; i++) {
        if (!offer->sections[i].is_data) continue;
        char fallback[sizeof(size_t) * 3 + 1];
        const char *mid = offer->sections[i].mid;
        if (!mid[0]) {
            snprintf(fallback, sizeof(fallback), "%zu", i);
            mid = fallback;
        }
        size_t used = 0;
        if (sdp_append(bundle, sizeof(bundle), &used, "%s", mid) < 0) return -1;
        break;
    }

    size_t off = 0;
    if (sdp_append(out, cap, &off,
                   "v=0\r\no=- %llu 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE %s\r\n",
                   session_id, bundle) < 0)
        return -1;

    const char *setup = strcasecmp(offer->setup, "passive") == 0 ? "active" : "passive";
    int accepted = 0;
    for (size_t i = 0; i < offer->section_count; i++) {
        const cwist_sdp_section *section = &offer->sections[i];
        char fallback[sizeof(size_t) * 3 + 1];
        const char *mid = section->mid;
        if (!mid[0]) {
            snprintf(fallback, sizeof(fallback), "%zu", i);
            mid = fallback;
        }
        if (section->is_data && accepted == 0) {
            if (sdp_append(out, cap, &off,
                           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                           "c=IN IP4 0.0.0.0\r\n"
                           "a=mid:%s\r\n"
                           "a=ice-ufrag:%s\r\n"
                           "a=ice-pwd:%s\r\n"
                           "a=ice-lite\r\n"
                           "a=fingerprint:sha-256 %s\r\n"
                           "a=setup:%s\r\n"
                           "a=candidate:1 1 UDP 2122260223 %s %u typ host\r\n"
                           "a=end-of-candidates\r\n"
                           "a=sctp-port:5000\r\n"
                           "a=max-message-size:262144\r\n",
                           mid, ufrag, pwd, fingerprint, setup, host, (unsigned)port) < 0)
                return -1;
            accepted++;
        } else if (sdp_append(out, cap, &off, "m=%s 0 %s %s\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\n",
                              section->media, section->proto, section->fmt, mid) < 0) {
            return -1;
        }
    }
    return accepted == 1 ? 0 : -1;
}

int cwist_sdp_write_answer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                           const char *pwd, const char *mid, const char *host, uint16_t port) {
    cwist_sdp_info offer = {0};
    offer.section_count = 1;
    snprintf(offer.sections[0].media, sizeof(offer.sections[0].media), "application");
    snprintf(offer.sections[0].proto, sizeof(offer.sections[0].proto), "UDP/DTLS/SCTP");
    snprintf(offer.sections[0].fmt, sizeof(offer.sections[0].fmt), "webrtc-datachannel");
    snprintf(offer.sections[0].mid, sizeof(offer.sections[0].mid), "%s", mid);
    offer.sections[0].is_data = 1;
    return cwist_sdp_write_answer_for_offer(out, cap, fingerprint, ufrag, pwd, &offer, host, port);
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
