/** @file ice.c
 * @brief Minimal STUN/ICE (RFC 5389 / RFC 8445) for the ICE-lite agent.
 *
 * Implements just enough of STUN message parsing, validation, and construction
 * to run an ICE-lite endpoint: binding request/response handling,
 * MESSAGE-INTEGRITY (HMAC-SHA1) and FINGERPRINT (CRC-32) generation and
 * checking, XOR-MAPPED-ADDRESS extraction, and random ICE credential
 * generation. No retransmission or state machine logic lives here; callers in
 * webrtc.c drive timing and nomination.
 */
#include "webrtc_internal.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>

/** @name STUN wire-format constants (RFC 5389)
 * @{ */
#define STUN_MAGIC 0x2112A442u /**< Magic cookie; also the XOR mask for addresses/ports. */
#define STUN_HDR_LEN 20        /**< Fixed STUN message header size in bytes. */
#define ATTR_MAPPED_ADDRESS 0x0001     /**< MAPPED-ADDRESS attribute type. Unused (we send XOR-MAPPED-ADDRESS). */
#define ATTR_USERNAME 0x0006           /**< USERNAME attribute type ("remoteufrag:localufrag"). */
#define ATTR_MESSAGE_INTEGRITY 0x0008  /**< MESSAGE-INTEGRITY attribute type (HMAC-SHA1, 20 bytes). */
#define ATTR_PRIORITY 0x0024           /**< PRIORITY attribute type (ICE, RFC 8445). */
#define ATTR_USE_CANDIDATE 0x0025      /**< USE-CANDIDATE attribute type (nomination hint). */
#define ATTR_XOR_MAPPED_ADDRESS 0x0020 /**< XOR-MAPPED-ADDRESS attribute type. */
#define ATTR_FINGERPRINT 0x8028        /**< FINGERPRINT attribute type (CRC-32 xor 0x5354554E). */
#define ATTR_ICE_CONTROLLED 0x8029     /**< ICE-CONTROLLED attribute type. Unused by this agent. */
#define ATTR_ICE_CONTROLLING 0x802A    /**< ICE-CONTROLLING attribute type (we always send it). */
/** @} */

/** Byte-at-a-time CRC-32 (IEEE, poly 0xEDB88320) lookup table, built lazily by crc32_init(). */
static uint32_t crc32_table[256];
/** Set once crc32_table has been initialized. */
static bool crc32_ready = false;

/** @brief Build the CRC-32 lookup table on first use.
 *
 * Idempotent; subsequent calls return immediately.
 */
static void crc32_init(void) {
    if (crc32_ready)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

/** @brief Incremental CRC-32 (IEEE) over possibly several consecutive calls.
 * @param c    Running CRC value; start a new checksum with 0xFFFFFFFF and xor
 *             the final result with 0xFFFFFFFF (the STUN FINGERPRINT framing).
 * @param data Bytes to fold into the checksum.
 * @param len  Number of bytes at @p data.
 * @return Updated running CRC.
 */
static uint32_t crc32_update(uint32_t c, const uint8_t *data, size_t len) {
    crc32_init();
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
}

/** @brief Read a 32-bit big-endian value.
 * @param p Pointer to at least 4 readable bytes.
 * @return The value in host byte order semantics (as an integer composed from big-endian bytes).
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/** @brief Read a 16-bit big-endian value.
 * @param p Pointer to at least 2 readable bytes.
 */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/** @brief Write a 16-bit value in big-endian order.
 * @param p Destination buffer (2 bytes).
 * @param v Value to encode.
 */
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/** @brief Write a 32-bit value in big-endian order.
 * @param p Destination buffer (4 bytes).
 * @param v Value to encode.
 */
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** @brief Check whether a datagram looks like a STUN message.
 * @param buf Datagram bytes.
 * @param len Number of bytes at @p buf.
 * @return 1 if @p buf holds at least a STUN header and the magic cookie matches,
 *         0 otherwise.
 * @note This is a shape check only; no attribute parsing or auth is performed.
 */
int cwist_ice_stun_is_message(const uint8_t *buf, size_t len) {
    return len >= STUN_HDR_LEN && rd32(buf + 4) == STUN_MAGIC;
}

/** @brief Check whether a datagram is a STUN binding request.
 * @param buf Datagram bytes.
 * @param len Number of bytes at @p buf.
 * @return true if @p buf is a STUN message with type CWIST_STUN_BINDING_REQUEST.
 */
bool cwist_ice_stun_is_binding_request(const uint8_t *buf, size_t len) {
    return cwist_ice_stun_is_message(buf, len) && rd16(buf) == CWIST_STUN_BINDING_REQUEST;
}

/** Callback invoked once per STUN attribute during a walk_attrs() scan.
 * @param type    Attribute type field.
 * @param val     Pointer to the attribute value inside the message buffer.
 * @param val_len Value length in bytes (unpadded).
 * @param arg     Opaque pointer supplied to walk_attrs().
 * @retval true   Continue walking.
 * @retval false  Stop walking (walk_attrs() then reports failure to its caller).
 */
typedef bool (*attr_cb)(uint16_t type, const uint8_t *val, uint16_t val_len, void *arg);

/** @brief Walk the attribute list of a STUN message.
 * @param msg STUN message buffer (header + attributes).
 * @param len Total bytes available at @p msg.
 * @param cb  Callback invoked for each attribute, in message order.
 * @param arg Passed through to @p cb.
 * @return true if the attribute area is well-formed and fully consumed,
 *         false if the message is truncated, malformed, or @p cb stopped the walk.
 */
static bool walk_attrs(const uint8_t *msg, size_t len, attr_cb cb, void *arg) {
    uint16_t msg_len = rd16(msg + 2);
    if ((size_t)msg_len + STUN_HDR_LEN > len || msg_len % 4 != 0)
        return false;
    size_t off = STUN_HDR_LEN;
    size_t end = STUN_HDR_LEN + msg_len;
    while (off + 4 <= end) {
        uint16_t type = rd16(msg + off);
        uint16_t alen = rd16(msg + off + 2);
        off += 4;
        if (off + alen > end)
            return false;
        if (!cb(type, msg + off, alen, arg))
            return false;
        off += (alen + 3) & ~3u;
    }
    return off == end;
}

/** State for attr_find_cb(): which attribute type to look for and the result. */
typedef struct {
    bool found;           /**< Set true once the wanted attribute has been seen. */
    uint16_t val_len;     /**< Value length of the found attribute (valid only if found). */
    uint16_t type_wanted; /**< Attribute type to search for. */
} attr_find_arg;

/** @brief attr_cb that records the first occurrence of a wanted attribute.
 * Stops the walk (returns false) after the first match so later duplicates are ignored.
 */
static bool attr_find_cb(uint16_t type, const uint8_t *val, uint16_t val_len, void *arg) {
    (void)val;
    attr_find_arg *a = arg;
    if (type == a->type_wanted) {
        a->found = true;
        a->val_len = val_len;
        return false;
    }
    return true;
}

/** @brief Check whether a STUN message contains an attribute of the given type.
 * @param msg  STUN message buffer.
 * @param len  Bytes available at @p msg.
 * @param type Attribute type to look for.
 * @return true if present. Malformed messages simply report "not found".
 */
static bool attr_present(const uint8_t *msg, size_t len, uint16_t type) {
    attr_find_arg a = { .type_wanted = type };
    walk_attrs(msg, len, attr_find_cb, &a);
    return a.found;
}

/** @brief Check whether a STUN message carries the USE-CANDIDATE attribute.
 * @param buf Datagram bytes.
 * @param len Number of bytes at @p buf.
 */
bool cwist_ice_stun_has_use_candidate(const uint8_t *buf, size_t len) {
    return attr_present(buf, len, ATTR_USE_CANDIDATE);
}

/** @brief Locate the MESSAGE-INTEGRITY attribute in a STUN message.
 * @param msg    STUN message buffer.
 * @param len    Bytes available at @p msg.
 * @param mi_off Receives the offset of the MI attribute header within @p msg.
 * @return true if a MESSAGE-INTEGRITY attribute was found, false otherwise.
 * @retval true  Only if the attribute length is exactly 20 (HMAC-SHA1 size).
 * @warning A short/long MI attribute is reported as "not found" here; callers
 *          treat that as an unsigned message rather than as a hard error.
 */
static bool find_mi(const uint8_t *msg, size_t len, size_t *mi_off) {
    if (!cwist_ice_stun_is_message(msg, len))
        return false;
    uint16_t msg_len = rd16(msg + 2);
    size_t end = STUN_HDR_LEN + msg_len;
    size_t off = STUN_HDR_LEN;
    while (off + 4 <= end) {
        uint16_t type = rd16(msg + off);
        uint16_t alen = rd16(msg + off + 2);
        if (type == ATTR_MESSAGE_INTEGRITY) {
            *mi_off = off;
            return alen == 20;
        }
        off += 4 + ((alen + 3) & ~3u);
    }
    return false;
}

/** @brief Validate the MESSAGE-INTEGRITY of a STUN message against a password.
 * @param buf Datagram bytes (any STUN message type; also used for responses).
 * @param len Number of bytes at @p buf.
 * @param pwd Short-term credential password used as the HMAC-SHA1 key.
 * @return 1 if the HMAC matches, 0 if the message is malformed, the HMAC does
 *         not match, or OpenSSL allocation fails.
 * @retval 1 Also when no MESSAGE-INTEGRITY attribute is present: accepted for
 *            MVP interoperability (RFC 8445 requires MI, but the agent stays
 *            permissive here).
 * @note The HMAC covers the message up to and including the MI attribute
 *       header, with the header's length field adjusted to end at the MI
 *       attribute, per RFC 5389.
 */
int cwist_ice_stun_validate_request(const uint8_t *buf, size_t len, const char *pwd) {
    size_t pwd_len = strlen(pwd);
    uint8_t hmac[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    size_t mi_off;
    if (!cwist_ice_stun_is_message(buf, len))
        return 0;
    if (!find_mi(buf, len, &mi_off))
        return 1; /* no MI: accept (RFC 8445 requires it, but stay permissive at MVP) */
    /* MI covers the message up to and including the MI attribute header, with
     * the message length field set to end at the MI attribute. */
    uint8_t hdr[STUN_HDR_LEN];
    memcpy(hdr, buf, STUN_HDR_LEN);
    wr16(hdr + 2, (uint16_t)(mi_off - STUN_HDR_LEN + 4 + 20));
    HMAC_CTX *ctx = HMAC_CTX_new();
    if (!ctx)
        return 0;
    HMAC_Init_ex(ctx, pwd, (int)pwd_len, EVP_sha1(), NULL);
    HMAC_Update(ctx, hdr, STUN_HDR_LEN);
    HMAC_Update(ctx, buf + STUN_HDR_LEN, mi_off - STUN_HDR_LEN);
    HMAC_Final(ctx, hmac, &hmac_len);
    HMAC_CTX_free(ctx);
    return hmac_len == 20 && memcmp(hmac, buf + mi_off + 4, 20) == 0;
}

/** @brief Build a STUN binding response for a received binding request.
 *
 * Emits a message containing XOR-MAPPED-ADDRESS (IPv4 only), MESSAGE-INTEGRITY
 * (HMAC-SHA1 keyed with @p pwd), and FINGERPRINT attributes, reusing the
 * request's transaction ID.
 *
 * @param out    Destination buffer.
 * @param cap    Capacity of @p out in bytes.
 * @param req    The received binding request (used for its transaction ID).
 * @param req_len Bytes at @p req; currently unused.
 * @param mapped Address/port to report back (XOR-encoded with the magic cookie).
 * @param pwd    Short-term credential password for the MI HMAC.
 * @return Total response length in bytes, or -1 if @p cap is too small or
 *         OpenSSL allocation fails.
 */
int cwist_ice_stun_build_response(uint8_t *out, size_t cap, const uint8_t *req, size_t req_len,
                                  const struct sockaddr_in *mapped, const char *pwd) {
    (void)req_len;
    uint8_t body[64];
    size_t blen = 0;

    /* XOR-MAPPED-ADDRESS, IPv4 */
    wr16(body + blen, ATTR_XOR_MAPPED_ADDRESS);
    wr16(body + blen + 2, 8);
    blen += 4;
    body[blen++] = 0x00;
    body[blen++] = 0x01; /* family IPv4 */
    wr16(body + blen, (uint16_t)(ntohs(mapped->sin_port) ^ 0xFFFF));
    blen += 2;
    uint32_t addr = ntohl(mapped->sin_addr.s_addr) ^ STUN_MAGIC;
    wr32(body + blen, addr);
    blen += 4;
    while (blen % 4)
        body[blen++] = 0;

    /* MESSAGE-INTEGRITY over header+body+MI attr header with adjusted length */
    uint16_t len_for_mi = (uint16_t)(blen + 4 + 20);
    uint8_t hdr[STUN_HDR_LEN];
    memset(hdr, 0, STUN_HDR_LEN);
    wr16(hdr, CWIST_STUN_BINDING_RESPONSE);
    wr16(hdr + 2, len_for_mi);
    wr32(hdr + 4, STUN_MAGIC);
    memcpy(hdr + 8, req + 8, 12);
    uint8_t mi[20];
    unsigned int mi_len = 0;
    HMAC_CTX *hctx = HMAC_CTX_new();
    if (!hctx)
        return -1;
    HMAC_Init_ex(hctx, pwd, (int)strlen(pwd), EVP_sha1(), NULL);
    HMAC_Update(hctx, hdr, STUN_HDR_LEN);
    HMAC_Update(hctx, body, blen);
    HMAC_Final(hctx, mi, &mi_len);
    HMAC_CTX_free(hctx);

    wr16(body + blen, ATTR_MESSAGE_INTEGRITY);
    wr16(body + blen + 2, 20);
    blen += 4;
    memcpy(body + blen, mi, 20);
    blen += 20;

    /* FINGERPRINT */
    uint16_t len_for_fp = (uint16_t)(blen + 8);
    uint8_t hdr2[STUN_HDR_LEN];
    memcpy(hdr2, hdr, STUN_HDR_LEN);
    wr16(hdr2 + 2, len_for_fp);
    uint32_t c = crc32_update(0xFFFFFFFFu, hdr2, STUN_HDR_LEN);
    c = crc32_update(c, body, blen);
    c ^= 0xFFFFFFFFu;
    wr16(body + blen, ATTR_FINGERPRINT);
    wr16(body + blen + 2, 4);
    wr32(body + blen + 4, c ^ 0x5354554Eu);
    blen += 8;

    if (cap < STUN_HDR_LEN + blen)
        return -1;
    memcpy(out, hdr2, STUN_HDR_LEN);
    memcpy(out + STUN_HDR_LEN, body, blen);
    return (int)(STUN_HDR_LEN + blen);
}

/** @brief Build an unsigned ICE-controlling STUN binding request.
 *
 * The request carries USERNAME (only if @p username is non-empty),
 * ICE-CONTROLLING, USE-CANDIDATE, and PRIORITY attributes. MESSAGE-INTEGRITY
 * and FINGERPRINT are intentionally not added here; the MI key (the peer's
 * password) is only known at send time, so the caller signs the built request
 * with cwist_ice_stun_sign_request().
 *
 * @param out      Destination buffer.
 * @param cap      Capacity of @p out in bytes.
 * @param txid     12-byte transaction ID to copy into the header.
 * @param username "remoteufrag:localufrag" string for the USERNAME attribute;
 *                 skipped when empty.
 * @return Total request length in bytes, or -1 if @p cap is too small.
 */
int cwist_ice_stun_build_request(uint8_t *out, size_t cap, const uint8_t txid[12],
                                 const char *username) {
    size_t ulen = strlen(username);
    if (cap < STUN_HDR_LEN)
        return -1;
    memset(out, 0, STUN_HDR_LEN);
    wr16(out, CWIST_STUN_BINDING_REQUEST);
    wr32(out + 4, STUN_MAGIC);
    memcpy(out + 8, txid, 12);

    size_t blen = 0;
    uint8_t body[256];
    /* SOFTWARE-ish placeholder skipped; USERNAME */
    if (ulen > 0) {
        wr16(body + blen, ATTR_USERNAME);
        wr16(body + blen + 2, (uint16_t)ulen);
        blen += 4;
        memcpy(body + blen, username, ulen);
        blen += ulen;
        while (blen % 4)
            body[blen++] = 0;
    }
    /* ICE-CONTROLLING (8 bytes) */
    wr16(body + blen, ATTR_ICE_CONTROLLING);
    wr16(body + blen + 2, 8);
    memset(body + blen + 4, 0, 8);
    blen += 12;
    /* USE-CANDIDATE */
    wr16(body + blen, ATTR_USE_CANDIDATE);
    wr16(body + blen + 2, 0);
    blen += 4;
    /* PRIORITY */
    wr16(body + blen, ATTR_PRIORITY);
    wr16(body + blen + 2, 4);
    wr32(body + blen + 4, 0x7EFFFFFFu);
    blen += 8;

    /* MESSAGE-INTEGRITY + FINGERPRINT added by the caller via _finalize; here we
     * keep the request unsigned because the MI key (peer pwd) is supplied at
     * send time. cwist_ice_stun_sign_request() handles it. */
    wr16(out + 2, (uint16_t)blen);
    if (cap < STUN_HDR_LEN + blen)
        return -1;
    memcpy(out + STUN_HDR_LEN, body, blen);
    return (int)(STUN_HDR_LEN + blen);
}

/** @brief Append MESSAGE-INTEGRITY and FINGERPRINT attributes to a built request.
 *
 * Rewrites the message header with the final length after appending. The MI
 * HMAC covers header+body up to and including the MI attribute header, with the
 * length field adjusted accordingly, per RFC 5389; the FINGERPRINT CRC covers
 * the whole message including MI.
 *
 * @param msg     Buffer holding a message previously built by
 *                cwist_ice_stun_build_request(); extended in place.
 * @param cap     Total capacity of @p msg.
 * @param msg_len Current length of the message in @p msg.
 * @param pwd     Short-term credential password for the MI HMAC.
 * @return New total message length, or -1 if @p cap is too small or OpenSSL
 *         allocation fails.
 */
static int stun_sign(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd) {
    uint8_t body[64];
    size_t blen = 0;
    uint16_t len_for_mi = (uint16_t)(msg_len - STUN_HDR_LEN + 4 + 20);
    uint8_t hdr[STUN_HDR_LEN];
    memcpy(hdr, msg, STUN_HDR_LEN);
    wr16(hdr + 2, len_for_mi);
    uint8_t mi[20];
    unsigned int mi_len = 0;
    HMAC_CTX *hctx = HMAC_CTX_new();
    if (!hctx)
        return -1;
    HMAC_Init_ex(hctx, pwd, (int)strlen(pwd), EVP_sha1(), NULL);
    HMAC_Update(hctx, hdr, STUN_HDR_LEN);
    HMAC_Update(hctx, msg + STUN_HDR_LEN, msg_len - STUN_HDR_LEN);
    HMAC_Final(hctx, mi, &mi_len);
    HMAC_CTX_free(hctx);

    wr16(body + blen, ATTR_MESSAGE_INTEGRITY);
    wr16(body + blen + 2, 20);
    blen += 4;
    memcpy(body + blen, mi, 20);
    blen += 20;

    uint16_t len_for_fp = (uint16_t)(msg_len - STUN_HDR_LEN + blen + 8);
    uint8_t hdr2[STUN_HDR_LEN];
    memcpy(hdr2, hdr, STUN_HDR_LEN);
    wr16(hdr2 + 2, len_for_fp);
    uint32_t c = crc32_update(0xFFFFFFFFu, hdr2, STUN_HDR_LEN);
    c = crc32_update(c, msg + STUN_HDR_LEN, msg_len - STUN_HDR_LEN);
    c = crc32_update(c, body, blen);
    c ^= 0xFFFFFFFFu;
    wr16(body + blen, ATTR_FINGERPRINT);
    wr16(body + blen + 2, 4);
    wr32(body + blen + 4, c ^ 0x5354554Eu);
    blen += 8;

    if (cap < msg_len + blen)
        return -1;
    memcpy(msg + msg_len, body, blen);
    memcpy(msg, hdr2, STUN_HDR_LEN);
    return (int)(msg_len + blen);
}

/** @brief Public wrapper around stun_sign() for signing a built binding request.
 * @param msg     Buffer holding the request; extended in place.
 * @param cap     Total capacity of @p msg.
 * @param msg_len Current request length.
 * @param pwd     Peer password used as the MI HMAC key.
 * @return New total length, or -1 on failure (see stun_sign()).
 */
int cwist_ice_stun_sign_request(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd) {
    return stun_sign(msg, cap, msg_len, pwd);
}

/** State for parse_resp_cb(): collected MESSAGE-INTEGRITY value and mapped address. */
typedef struct {
    const uint8_t *mi_val;   /**< Pointer to the MI attribute value (into the message buffer), or NULL. */
    struct sockaddr_in mapped; /**< Decoded XOR-MAPPED-ADDRESS, valid only if has_mapped. */
    bool has_mapped;         /**< True once an IPv4 XOR-MAPPED-ADDRESS has been decoded. */
} parse_resp_arg;

/** @brief attr_cb collecting the MI value and XOR-MAPPED-ADDRESS from a response.
 * Never stops the walk; duplicates overwrite earlier values (last one wins).
 */
static bool parse_resp_cb(uint16_t type, const uint8_t *val, uint16_t val_len, void *arg) {
    parse_resp_arg *a = arg;
    if (type == ATTR_MESSAGE_INTEGRITY) {
        a->mi_val = val;
        (void)val_len;
    } else if (type == ATTR_XOR_MAPPED_ADDRESS && val_len >= 8) {
        a->mapped.sin_family = AF_INET;
        a->mapped.sin_port = htons((uint16_t)(rd16(val + 2) ^ 0xFFFF));
        a->mapped.sin_addr.s_addr = htonl(rd32(val + 4) ^ STUN_MAGIC);
        a->has_mapped = true;
    }
    return true;
}

/** @brief Validate a received STUN binding response.
 *
 * Checks, in order: well-formed STUN message of type BINDING-RESPONSE,
 * transaction ID match against @p txid, MESSAGE-INTEGRITY HMAC with @p pwd
 * (via cwist_ice_stun_validate_request()), and presence of a MESSAGE-INTEGRITY
 * attribute. If @p mapped is non-NULL, an IPv4 XOR-MAPPED-ADDRESS must also be
 * present and is decoded into it.
 *
 * @param buf    Datagram bytes.
 * @param len    Number of bytes at @p buf.
 * @param txid   Transaction ID the response must carry (the one we sent).
 * @param pwd    Local password used as the MI HMAC key.
 * @param mapped Optional output for the decoded mapped address.
 * @retval 1 Valid response.
 * @retval 0 Any check failed or the message is malformed.
 */
int cwist_ice_stun_parse_response(const uint8_t *buf, size_t len, const uint8_t txid[12],
                                  const char *pwd, struct sockaddr_in *mapped) {
    if (!cwist_ice_stun_is_message(buf, len) || rd16(buf) != CWIST_STUN_BINDING_RESPONSE)
        return 0;
    if (memcmp(buf + 8, txid, 12) != 0)
        return 0;
    if (!cwist_ice_stun_validate_request(buf, len, pwd))
        return 0;
    parse_resp_arg a = { 0 };
    if (!walk_attrs(buf, len, parse_resp_cb, &a))
        return 0;
    if (!a.mi_val)
        return 0;
    if (mapped) {
        if (!a.has_mapped)
            return 0;
        *mapped = a.mapped;
    }
    return 1;
}

/** Base64url alphabet (no padding) used to encode random ICE credentials. */
static const char b64url_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/** @brief Extract the remote ufrag from the USERNAME attribute of a STUN message.
 *
 * The USERNAME attribute holds "remoteufrag:localufrag" (RFC 8445); this
 * returns the part before the first ':'.
 *
 * @param buf Datagram bytes.
 * @param len Number of bytes at @p buf.
 * @param out Output buffer for the NUL-terminated ufrag.
 * @param cap Capacity of @p out; output is truncated to cap - 1 characters.
 * @retval 0 Ufrag extracted (possibly truncated).
 * @retval -1 Not a STUN message, or no USERNAME attribute present.
 */
int cwist_ice_stun_get_remote_ufrag(const uint8_t *buf, size_t len, char *out, size_t cap) {
    if (!cwist_ice_stun_is_message(buf, len))
        return -1;
    uint16_t msg_len = rd16(buf + 2);
    size_t end = STUN_HDR_LEN + msg_len;
    size_t off = STUN_HDR_LEN;
    while (off + 4 <= end) {
        uint16_t type = rd16(buf + off);
        uint16_t alen = rd16(buf + off + 2);
        if (type == ATTR_USERNAME) {
            size_t i = 0;
            while (i < alen && i + 1 < cap && buf[off + 4 + i] != ':') {
                out[i] = (char)buf[off + 4 + i];
                i++;
            }
            out[i] = '\0';
            return 0;
        }
        off += 4 + ((alen + 3) & ~3u);
    }
    return -1;
}

/** @brief Generate random ICE ufrag/pwd credentials.
 *
 * Both strings are drawn from 24 bytes of CSPRNG output encoded with the
 * base64url alphabet (6 bits per character).
 *
 * @param ufrag     Output buffer for the ufrag.
 * @param ufrag_cap Capacity of @p ufrag; up to 8 characters plus NUL are written.
 * @param pwd       Output buffer for the password.
 * @param pwd_cap   Capacity of @p pwd; up to 32 characters plus NUL are written.
 * @warning Behavior is undefined if either capacity is 0 (cap - 1 underflows).
 */
void cwist_ice_random_creds(char *ufrag, size_t ufrag_cap, char *pwd, size_t pwd_cap) {
    uint8_t raw[24];
    RAND_bytes(raw, sizeof(raw));
    size_t n = ufrag_cap - 1 < 8 ? ufrag_cap - 1 : 8;
    for (size_t i = 0; i < n; i++)
        ufrag[i] = b64url_chars[raw[i] & 63];
    ufrag[n] = '\0';
    size_t m = pwd_cap - 1 < 32 ? pwd_cap - 1 : 32;
    for (size_t i = 0; i < m; i++)
        pwd[i] = b64url_chars[raw[8 + (i % 16)] & 63];
    pwd[m] = '\0';
}
