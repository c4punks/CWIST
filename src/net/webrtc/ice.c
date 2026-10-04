/** @file ice.c
 * @brief Minimal STUN/ICE (RFC 5389 / RFC 8445) for the ICE-lite agent.
 */
#include "webrtc_internal.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>

#define STUN_MAGIC 0x2112A442u
#define STUN_HDR_LEN 20
#define ATTR_MAPPED_ADDRESS 0x0001
#define ATTR_USERNAME 0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_PRIORITY 0x0024
#define ATTR_USE_CANDIDATE 0x0025
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_FINGERPRINT 0x8028
#define ATTR_ICE_CONTROLLED 0x8029
#define ATTR_ICE_CONTROLLING 0x802A

static uint32_t crc32_table[256];
static bool crc32_ready = false;

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

/* Incremental CRC-32 (IEEE) over possibly several consecutive calls. */
static uint32_t crc32_update(uint32_t c, const uint8_t *data, size_t len) {
    crc32_init();
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int cwist_ice_stun_is_message(const uint8_t *buf, size_t len) {
    return len >= STUN_HDR_LEN && rd32(buf + 4) == STUN_MAGIC;
}

bool cwist_ice_stun_is_binding_request(const uint8_t *buf, size_t len) {
    return cwist_ice_stun_is_message(buf, len) && rd16(buf) == CWIST_STUN_BINDING_REQUEST;
}

/* Walk attributes. Calls cb(type, value, value_len) for each; stop if cb returns false. */
typedef bool (*attr_cb)(uint16_t type, const uint8_t *val, uint16_t val_len, void *arg);

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

typedef struct {
    bool found;
    uint16_t val_len;
    uint16_t type_wanted;
} attr_find_arg;

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

static bool attr_present(const uint8_t *msg, size_t len, uint16_t type) {
    attr_find_arg a = { .type_wanted = type };
    walk_attrs(msg, len, attr_find_cb, &a);
    return a.found;
}

bool cwist_ice_stun_has_use_candidate(const uint8_t *buf, size_t len) {
    return attr_present(buf, len, ATTR_USE_CANDIDATE);
}

/* Locate the MESSAGE-INTEGRITY attribute; *mi_off gets its header offset. */
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

/* Append MI + FINGERPRINT to a freshly built request. */
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

int cwist_ice_stun_sign_request(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd) {
    return stun_sign(msg, cap, msg_len, pwd);
}

typedef struct {
    const uint8_t *mi_val;
    struct sockaddr_in mapped;
    bool has_mapped;
} parse_resp_arg;

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

static const char b64url_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

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
