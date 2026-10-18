#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/seq/seq.h>

#include <cjson/cJSON.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/obj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @file jwt.c
 * @brief HS256/ES256/EdDSA JWT signing, verification, and decoded-claims access helpers.
 */

/* --------------------------------------------------------------------------
 * Internal: Base64URL helpers
 * -------------------------------------------------------------------------- */

/** Return the Base64URL-encoded length for @p input_len raw bytes. */
static size_t b64url_encoded_len(size_t input_len) {
    return ((input_len + 2) / 3) * 4 + 1; /* +1 for '\0' */
}

/** Encode @p src_len bytes of @p src into @p dst (null-terminated). */
static void b64url_encode(const unsigned char *src, size_t src_len, char *dst) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0, j = 0;
    while (i + 2 < src_len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[j++] = tbl[(v >> 18) & 0x3F];
        dst[j++] = tbl[(v >> 12) & 0x3F];
        dst[j++] = tbl[(v >> 6) & 0x3F];
        dst[j++] = tbl[v & 0x3F];
        i += 3;
    }
    if (i + 1 == src_len) {
        uint32_t v = (uint32_t)src[i] << 4;
        dst[j++] = tbl[(v >> 6) & 0x3F];
        dst[j++] = tbl[v & 0x3F];
    } else if (i + 2 == src_len) {
        uint32_t v = ((uint32_t)src[i] << 10) | ((uint32_t)src[i + 1] << 2);
        dst[j++] = tbl[(v >> 12) & 0x3F];
        dst[j++] = tbl[(v >> 6) & 0x3F];
        dst[j++] = tbl[v & 0x3F];
    }
    dst[j] = '\0';
}

/**
 * @brief Decode a Base64URL string.
 * @param src Base64URL input; either standard or URL-safe alphabet, with optional '=' padding.
 * @param src_len Length of @p src in bytes.
 * @param out_len Set to the number of decoded bytes on success.
 * @return Heap-allocated decoded buffer, or NULL on invalid input or allocation failure.
 * @retval NULL Invalid input or allocation failure.
 *
 * Ownership of a non-NULL return passes to the caller; free with cwist_free().
 */
static unsigned char *b64url_decode(const char *src, size_t src_len, size_t *out_len) {
    /* Accept either standard or url-safe alphabet; strip '=' padding. */
    static const signed char lut[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
        -1, 62, -1, 63, /* '+' -> 62, '-' -> 62, '/' & '_' -> 63 */
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, 0,  -1, -1, /* '0'-'9', '=' -> 0 */
        -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, /* 'A'-'O' */
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63, /* 'P'-'Z', '_' -> 63 */
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, /* 'a'-'o' */
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, /* 'p'-'z' */
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };

    /* Calculate how many padding chars to expect */
    size_t padded_len = src_len;
    while (padded_len > 0 && src[padded_len - 1] == '=') {
        padded_len--;
    }

    size_t decoded_len = (padded_len * 3) / 4;
    unsigned char *out = (unsigned char *)cwist_alloc(decoded_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i + 3 < padded_len) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i + 1]];
        signed char c = lut[(unsigned char)src[i + 2]];
        signed char d = lut[(unsigned char)src[i + 3]];
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            cwist_free(out);
            return NULL;
        }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        out[j++] = (unsigned char)((b << 4) | (c >> 2));
        out[j++] = (unsigned char)((c << 6) | d);
        i += 4;
    }

    size_t rem = padded_len - i;
    if (rem == 2) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i + 1]];
        if (a < 0 || b < 0) {
            cwist_free(out);
            return NULL;
        }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
    } else if (rem == 3) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i + 1]];
        signed char c = lut[(unsigned char)src[i + 2]];
        if (a < 0 || b < 0 || c < 0) {
            cwist_free(out);
            return NULL;
        }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        out[j++] = (unsigned char)((b << 4) | (c >> 2));
    } else if (rem == 1) {
        /* RFC 4648 section 4: 1 base64 character (6 bits) cannot encode a whole byte.
         * A remainder of 1 is malformed and must be rejected. */
        cwist_free(out);
        return NULL;
    }

    out[j] = '\0';
    *out_len = j;
    return out;
}

/* --------------------------------------------------------------------------
 * Internal: algorithm dispatch (HS256, ES256, Ed25519)
 * -------------------------------------------------------------------------- */

typedef enum {
    JWT_ALG_HS256,
    JWT_ALG_ES256,
    JWT_ALG_ED25519,
} jwt_alg_t;

static const char *jwt_alg_name(jwt_alg_t alg) {
    switch (alg) {
    case JWT_ALG_HS256: return "HS256";
    case JWT_ALG_ES256: return "ES256";
    case JWT_ALG_ED25519: return "EdDSA";
    }
    return NULL;
}

/** Compute HMAC-SHA256 of @p msg using @p key; writes to @p out (32 bytes). */
static bool hmac_sha256(const char *key, size_t key_len, const char *msg, size_t msg_len,
                        unsigned char out[32]) {
    unsigned int out_len = 0;
    unsigned char *r =
        HMAC(EVP_sha256(), key, (int)key_len, (const unsigned char *)msg, msg_len, out, &out_len);
    return r != NULL && out_len == 32;
}

/** Load a PEM-encoded private key, or NULL. */
static EVP_PKEY *jwt_load_private_key(const char *pem) {
    BIO *bio = BIO_new_mem_buf(pem, (int)strlen(pem));
    if (!bio) return NULL;
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return key;
}

/** Load a PEM-encoded public key, falling back to a private key (public half used). */
static EVP_PKEY *jwt_load_public_key(const char *pem) {
    BIO *bio = BIO_new_mem_buf(pem, (int)strlen(pem));
    if (!bio) return NULL;
    EVP_PKEY *key = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    if (!key) {
        /* The first read consumed the buffer; rewind before the fallback. */
        BIO_reset(bio);
        key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    }
    BIO_free(bio);
    return key;
}

/** Whether @p key is an EC key on P-256, as ES256 requires. */
static bool jwt_key_is_p256(EVP_PKEY *key) {
    if (EVP_PKEY_id(key) != EVP_PKEY_EC) return false;
    EC_KEY *ec = EVP_PKEY_get0_EC_KEY(key);
    if (!ec) return false;
    const EC_GROUP *group = EC_KEY_get0_group(ec);
    return group && EC_GROUP_get_curve_name(group) == NID_X9_62_prime256v1;
}

/**
 * Sign @p msg with @p alg; @p key is the HMAC secret for HS256 or a PEM
 * private key otherwise. Returns a cwist_alloc'd signature in JOSE form
 * (raw R||S for ES256), or NULL on error.
 */
static unsigned char *jwt_sign_bytes(jwt_alg_t alg, const char *key, const char *msg,
                                     size_t msg_len, size_t *out_len) {
    if (alg == JWT_ALG_HS256) {
        unsigned char *sig = (unsigned char *)cwist_alloc(32);
        if (!sig) return NULL;
        if (!hmac_sha256(key, strlen(key), msg, msg_len, sig)) {
            cwist_free(sig);
            return NULL;
        }
        *out_len = 32;
        return sig;
    }

    EVP_PKEY *pkey = jwt_load_private_key(key);
    if (!pkey) return NULL;
    if (alg == JWT_ALG_ES256 && !jwt_key_is_p256(pkey)) {
        EVP_PKEY_free(pkey);
        return NULL;
    }
    if (alg == JWT_ALG_ED25519 && EVP_PKEY_id(pkey) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(pkey);
        return NULL;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return NULL;
    }
    const EVP_MD *md = (alg == JWT_ALG_ES256) ? EVP_sha256() : NULL;
    unsigned char *sig = NULL;
    if (EVP_DigestSignInit(ctx, NULL, md, NULL, pkey) != 1) goto fail;

    size_t raw_len = 0;
    if (EVP_DigestSign(ctx, NULL, &raw_len, (const unsigned char *)msg, msg_len) != 1) goto fail;
    {
        unsigned char *raw = (unsigned char *)cwist_alloc(raw_len);
        if (!raw) goto fail;
        if (EVP_DigestSign(ctx, raw, &raw_len, (const unsigned char *)msg, msg_len) != 1) {
            cwist_free(raw);
            goto fail;
        }
        if (alg == JWT_ALG_ED25519) {
            sig = raw; /* Ed25519 signs directly in JOSE form (64 bytes) */
            *out_len = raw_len;
        } else {
            /* Convert the ASN.1 DER ECDSA signature to raw 32-byte R||S. */
            sig = (unsigned char *)cwist_alloc(64);
            if (!sig) {
                cwist_free(raw);
                goto fail;
            }
            ECDSA_SIG *ecsig = ECDSA_SIG_from_bytes(raw, raw_len);
            cwist_free(raw);
            if (!ecsig) {
                cwist_free(sig);
                sig = NULL;
                goto fail;
            }
            const BIGNUM *r, *s;
            ECDSA_SIG_get0(ecsig, &r, &s);
            if (BN_bn2bin_padded(sig, 32, r) != 1 ||
                BN_bn2bin_padded(sig + 32, 32, s) != 1) {
                ECDSA_SIG_free(ecsig);
                cwist_free(sig);
                sig = NULL;
                goto fail;
            }
            ECDSA_SIG_free(ecsig);
            *out_len = 64;
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;

fail:
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return NULL;
}

/**
 * Verify @p sig (JOSE form) over @p msg with @p alg; @p key is the HMAC
 * secret for HS256 or a PEM public/private key otherwise. The ES256 key must
 * be on P-256 and the Ed25519 key must be Ed25519, so a key of the wrong
 * type can never validate.
 */
static bool jwt_verify_bytes(jwt_alg_t alg, const char *key, const char *msg, size_t msg_len,
                             const unsigned char *sig, size_t sig_len) {

    if (alg == JWT_ALG_HS256) {
        unsigned char expected[32];
        if (sig_len != 32) return false;
        if (!hmac_sha256(key, strlen(key), msg, msg_len, expected)) return false;
        int cmp = 0;
        for (int i = 0; i < 32; i++) {
            cmp |= expected[i] ^ sig[i];
        }
        return cmp == 0;
    }

    bool ok = false;
    EVP_MD_CTX *ctx = NULL;
    EVP_PKEY *pkey = jwt_load_public_key(key);
    if (!pkey) return false;
    if (alg == JWT_ALG_ES256 && !jwt_key_is_p256(pkey)) goto fail;
    if (alg == JWT_ALG_ED25519 && EVP_PKEY_id(pkey) != EVP_PKEY_ED25519) goto fail;

    ctx = EVP_MD_CTX_new();
    if (!ctx) goto fail;
    const EVP_MD *md = (alg == JWT_ALG_ES256) ? EVP_sha256() : NULL;
    if (EVP_DigestVerifyInit(ctx, NULL, md, NULL, pkey) != 1) goto out_ctx;

    if (alg == JWT_ALG_ED25519) {
        ok = EVP_DigestVerify(ctx, sig, sig_len, (const unsigned char *)msg, msg_len) == 1;
    } else {
        /* Re-encode raw R||S as the ASN.1 DER the EVP interface expects. */
        if (sig_len != 64) goto out_ctx;
        BIGNUM *r = BN_bin2bn(sig, 32, NULL);
        BIGNUM *s = BN_bin2bn(sig + 32, 32, NULL);
        ECDSA_SIG *ecsig = ECDSA_SIG_new();
        if (!r || !s || !ecsig || ECDSA_SIG_set0(ecsig, r, s) != 1) {
            BN_free(r);
            BN_free(s);
            ECDSA_SIG_free(ecsig);
            goto out_ctx;
        }
        uint8_t *der = NULL;
        size_t der_len = 0;
        if (ECDSA_SIG_to_bytes(&der, &der_len, ecsig) == 1) {
            ok = EVP_DigestVerify(ctx, der, der_len, (const unsigned char *)msg, msg_len) == 1;
            OPENSSL_free(der);
        }
        ECDSA_SIG_free(ecsig); /* takes r and s */
    }

out_ctx:
    EVP_MD_CTX_free(ctx);
fail:
    EVP_PKEY_free(pkey);
    return ok;
}

/* --------------------------------------------------------------------------
 * JWT claims (opaque struct)
 * -------------------------------------------------------------------------- */

struct cwist_jwt_claims {
    cJSON *json; ///< Parsed payload JSON object.
};

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Sign a JSON payload as a JWT with @p alg and optionally inject exp/iat.
 * @param alg Signature algorithm (HS256/ES256/EdDSA).
 * @param payload_json Raw payload JSON string to sign.
 * @param key HMAC secret (HS256) or PEM private key (ES256/Ed25519).
 * @param kid Optional key id for the header, or NULL.
 * @param exp_seconds Lifetime in seconds to add when the payload lacks exp/iat.
 * @return Heap-allocated JWT string, or NULL on parse/signature allocation failure.
 */
static char *jwt_sign_common(jwt_alg_t alg, const char *payload_json, const char *key,
                             const char *kid, long exp_seconds) {
    if (!payload_json || !key) return NULL;

    /* --- Build header JSON ------------------------------------------------ */
    cJSON *header = cJSON_CreateObject();
    if (!header) return NULL;
    cJSON_AddStringToObject(header, "alg", jwt_alg_name(alg));
    cJSON_AddStringToObject(header, "typ", "JWT");
    if (kid) {
        cJSON_AddStringToObject(header, "kid", kid);
    }
    char *header_json = cJSON_PrintUnformatted(header);
    cJSON_Delete(header);
    if (!header_json) return NULL;

    /* --- Optionally inject exp/iat into payload --------------------------- */
    cJSON *payload = cJSON_Parse(payload_json);
    if (!payload) {
        cJSON_free(header_json);
        return NULL;
    }

    if (exp_seconds > 0) {
        time_t now = time(NULL);
        /* Only add if not already present */
        if (!cJSON_GetObjectItemCaseSensitive(payload, "iat")) {
            cJSON_AddNumberToObject(payload, "iat", (double)now);
        }
        if (!cJSON_GetObjectItemCaseSensitive(payload, "exp")) {
            cJSON_AddNumberToObject(payload, "exp", (double)(now + exp_seconds));
        }
    }

    char *final_payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!final_payload_json) {
        cJSON_free(header_json);
        return NULL;
    }

    /* --- Base64URL encode header and payload ------------------------------ */
    size_t hdr_enc_len = b64url_encoded_len(strlen(header_json));
    size_t pay_enc_len = b64url_encoded_len(strlen(final_payload_json));

    char *hdr_enc CWIST_DEFER_FREE = (char *)cwist_alloc(hdr_enc_len);
    char *pay_enc CWIST_DEFER_FREE = (char *)cwist_alloc(pay_enc_len);
    if (!hdr_enc || !pay_enc) {
        cJSON_free(header_json);
        cJSON_free(final_payload_json);
        return NULL;
    }

    b64url_encode((const unsigned char *)header_json, strlen(header_json), hdr_enc);
    b64url_encode((const unsigned char *)final_payload_json, strlen(final_payload_json), pay_enc);
    cJSON_free(header_json);
    cJSON_free(final_payload_json);

    /* --- Build "header.payload" signing input using sstring --------------- */
    cwist_sstring *signing_input = cwist_sstring_create();
    if (!signing_input) {
        return NULL;
    }
    cwist_sstring_append(signing_input, hdr_enc);
    cwist_sstring_append(signing_input, ".");
    cwist_sstring_append(signing_input, pay_enc);

    /* --- Compute the signature -------------------------------------------- */
    size_t sig_len = 0;
    unsigned char *sig_raw = jwt_sign_bytes(alg, key, signing_input->data, signing_input->size,
                                            &sig_len);
    if (!sig_raw) {
        cwist_sstring_destroy(signing_input);
        return NULL;
    }

    size_t sig_enc_len = b64url_encoded_len(sig_len);
    char *sig_enc CWIST_DEFER_FREE = (char *)cwist_alloc(sig_enc_len);
    if (!sig_enc) {
        cwist_free(sig_raw);
        cwist_sstring_destroy(signing_input);
        return NULL;
    }
    b64url_encode(sig_raw, sig_len, sig_enc);
    cwist_free(sig_raw);

    /* --- Assemble final token using sstring ------------------------------- */
    cwist_sstring *token = cwist_sstring_create();
    if (!token) {
        cwist_sstring_destroy(signing_input);
        return NULL;
    }
    cwist_sstring_append(token, signing_input->data);
    cwist_sstring_append(token, ".");
    cwist_sstring_append(token, sig_enc);

    char *result = cwist_strdup(token->data);

    cwist_sstring_destroy(token);
    cwist_sstring_destroy(signing_input);

    return result;
}

char *cwist_jwt_sign(const char *payload_json, const char *secret, long exp_seconds) {
    return jwt_sign_common(JWT_ALG_HS256, payload_json, secret, NULL, exp_seconds);
}

char *cwist_jwt_sign_es256(const char *payload_json, const char *pem_private_key, const char *kid,
                           long exp_seconds) {
    return jwt_sign_common(JWT_ALG_ES256, payload_json, pem_private_key, kid, exp_seconds);
}

char *cwist_jwt_sign_ed25519(const char *payload_json, const char *pem_private_key,
                             const char *kid, long exp_seconds) {
    return jwt_sign_common(JWT_ALG_ED25519, payload_json, pem_private_key, kid, exp_seconds);
}

/**
 * @brief Verify a JWT with @p alg, parse its payload, and enforce exp/nbf when present.
 * @param alg Signature algorithm the token header must declare exactly.
 * @param token JWT string in header.payload.signature form.
 * @param key HMAC secret (HS256) or PEM public/private key (ES256/Ed25519).
 * @return Heap-allocated claims object, or NULL when verification/parsing fails.
 */
static cwist_jwt_claims *jwt_verify_common(jwt_alg_t alg, const char *token, const char *key) {
    if (!token || !key) return NULL;

    /* --- Split the token into three parts --------------------------------- */
    /* We need mutable copies to split on '.' */
    char *tok_copy = cwist_strdup(token);
    if (!tok_copy) return NULL;

    char *dot1 = strchr(tok_copy, '.');
    if (!dot1) {
        cwist_free(tok_copy);
        return NULL;
    }
    *dot1 = '\0';

    char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        cwist_free(tok_copy);
        return NULL;
    }
    *dot2 = '\0';

    /* --- Validate the header: must declare exactly our alg (RFC 8725 3.1) - */
    size_t hdr_json_len = 0;
    unsigned char *hdr_json = b64url_decode(tok_copy, strlen(tok_copy), &hdr_json_len);
    if (!hdr_json) {
        cwist_free(tok_copy);
        return NULL;
    }
    cJSON *hdr = cJSON_ParseWithLength((const char *)hdr_json, hdr_json_len);
    cwist_free(hdr_json);
    if (!hdr) {
        cwist_free(tok_copy);
        return NULL;
    }
    cJSON *alg_item = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
    bool alg_ok = alg_item && cJSON_IsString(alg_item) &&
                  strcmp(alg_item->valuestring, jwt_alg_name(alg)) == 0;
    cJSON_Delete(hdr);
    if (!alg_ok) {
        cwist_free(tok_copy);
        return NULL;
    }

    const char *pay_enc = dot1 + 1;
    const char *sig_enc = dot2 + 1;

    /* --- Decode the provided signature ------------------------------------ */
    size_t provided_sig_len = 0;
    unsigned char *provided_sig = b64url_decode(sig_enc, strlen(sig_enc), &provided_sig_len);
    if (!provided_sig) {
        cwist_free(tok_copy);
        return NULL;
    }

    /* --- Verify over the original "hdr_enc.pay_enc" signing input --------- */
    size_t first_two_len = (size_t)(dot2 - tok_copy);
    bool sig_ok = jwt_verify_bytes(alg, key, token, first_two_len, provided_sig,
                                   provided_sig_len);
    cwist_free(provided_sig);
    if (!sig_ok) {
        cwist_free(tok_copy);
        return NULL; /* signature mismatch */
    }

    /* --- Decode and parse the payload ------------------------------------- */
    size_t pay_json_len = 0;
    unsigned char *pay_json = b64url_decode(pay_enc, strlen(pay_enc), &pay_json_len);
    cwist_free(tok_copy);

    if (!pay_json) return NULL;

    cJSON *json = cJSON_ParseWithLength((const char *)pay_json, pay_json_len);
    cwist_free(pay_json);

    if (!json) return NULL;

    /* --- Validate "exp" and "nbf" claims with leeway for clock skew/high-RTT --- */
#define CWIST_JWT_TIME_LEEWAY 300
    time_t now = time(NULL);
    cJSON *exp_item = cJSON_GetObjectItemCaseSensitive(json, "exp");
    if (exp_item && cJSON_IsNumber(exp_item)) {
        time_t exp_time = (time_t)exp_item->valuedouble;
        if (now > CWIST_JWT_TIME_LEEWAY && exp_time < now - CWIST_JWT_TIME_LEEWAY) {
            cJSON_Delete(json);
            return NULL; /* token expired */
        }
    }
    cJSON *nbf_item = cJSON_GetObjectItemCaseSensitive(json, "nbf");
    if (nbf_item && cJSON_IsNumber(nbf_item)) {
        time_t nbf_time = (time_t)nbf_item->valuedouble;
        if (nbf_time > now && (nbf_time - now) > CWIST_JWT_TIME_LEEWAY) {
            cJSON_Delete(json);
            return NULL; /* token not yet valid */
        }
    }

    /* --- Build and return claims object ----------------------------------- */
    cwist_jwt_claims *claims = (cwist_jwt_claims *)cwist_alloc(sizeof(cwist_jwt_claims));
    if (!claims) {
        cJSON_Delete(json);
        return NULL;
    }
    claims->json = json;
    return claims;
}

cwist_jwt_claims *cwist_jwt_verify(const char *token, const char *secret) {
    return jwt_verify_common(JWT_ALG_HS256, token, secret);
}

cwist_jwt_claims *cwist_jwt_verify_es256(const char *token, const char *pem_key) {
    return jwt_verify_common(JWT_ALG_ES256, token, pem_key);
}

cwist_jwt_claims *cwist_jwt_verify_ed25519(const char *token, const char *pem_key) {
    return jwt_verify_common(JWT_ALG_ED25519, token, pem_key);
}

/**
 * @brief Retrieve one string-valued claim from a verified JWT payload.
 * @param claims Verified claims object.
 * @param key Claim name to retrieve.
 * @return Borrowed pointer to the claim string, or NULL when absent/non-string.
 */
const char *cwist_jwt_claims_get(const cwist_jwt_claims *claims, const char *key) {
    if (!claims || !key) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(claims->json, key);
    if (!item) return NULL;
    if (cJSON_IsString(item)) return item->valuestring;
    /* For numeric claims return the raw JSON representation */
    return NULL;
}

/**
 * @brief Destroy a verified claims object and its parsed JSON payload.
 * @param claims Claims object to destroy.
 */
void cwist_jwt_claims_destroy(cwist_jwt_claims *claims) {
    if (!claims) return;
    cJSON_Delete(claims->json);
    cwist_free(claims);
}

/* --------------------------------------------------------------------------
 * Sequenced JWT transport helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Split a JWT token into sequenced transport chunks.
 * @param token Null-terminated JWT string to split.
 * @param chunk_payload_size Maximum payload bytes per chunk; must be non-zero.
 * @param out_count Set to the number of chunks produced.
 * @return Heap-allocated array of @p out_count chunks, or NULL on invalid input,
 *         split failure, or allocation failure.
 *
 * Each chunk's @c data buffer is owned by the caller and must be released with
 * cwist_jwt_chunks_free().
 */
cwist_jwt_chunk_t *cwist_jwt_split_chunks(const char *token, uint16_t chunk_payload_size,
                                          size_t *out_count) {
    if (!token || chunk_payload_size == 0 || !out_count) return NULL;
    *out_count = 0;

    size_t token_len = strlen(token);
    if (token_len == 0) return NULL;

    cwist_seq_message_t msg;
    if (!cwist_seq_split((const uint8_t *)token, token_len, chunk_payload_size, &msg)) {
        return NULL;
    }

    cwist_jwt_chunk_t *chunks =
        (cwist_jwt_chunk_t *)cwist_alloc_array(msg.count, sizeof(cwist_jwt_chunk_t));
    if (!chunks) {
        cwist_seq_message_free(&msg);
        return NULL;
    }

    for (size_t i = 0; i < msg.count; i++) {
        chunks[i].data = msg.chunks[i];
        chunks[i].len = msg.chunk_lens[i];
        msg.chunks[i] = NULL; /* ownership transferred */
    }

    size_t count = msg.count;
    cwist_seq_message_free(&msg);
    *out_count = count;
    return chunks;
}

/**
 * @brief Free a chunk array produced by cwist_jwt_split_chunks() or cwist_jwt_sign_chunks().
 * @param chunks Chunk array to free; NULL is accepted as a no-op.
 * @param count Number of entries in @p chunks.
 */
void cwist_jwt_chunks_free(cwist_jwt_chunk_t *chunks, size_t count) {
    if (!chunks) return;
    for (size_t i = 0; i < count; i++) cwist_free(chunks[i].data);
    cwist_free(chunks);
}

/**
 * @brief Reassemble a JWT token from sequenced transport chunks.
 * @param chunks Array of sequenced chunks, in any order accepted by the assembler.
 * @param count Number of entries in @p chunks.
 * @return Heap-allocated null-terminated JWT string, or NULL when the chunk set is
 *         invalid/incomplete or assembly/allocation fails.
 *
 * The caller owns the returned string and must free it with cwist_free().
 */
char *cwist_jwt_join_chunks(const cwist_jwt_chunk_t *chunks, size_t count) {
    if (!chunks || count == 0) return NULL;

    cwist_seq_assembler_t *a = cwist_seq_assembler_create();
    if (!a) return NULL;

    for (size_t i = 0; i < count; i++) {
        cwist_seq_chunk_t chunk;
        if (!cwist_seq_chunk_parse(chunks[i].data, chunks[i].len, &chunk)) {
            cwist_seq_assembler_destroy(a);
            return NULL;
        }
        if (!cwist_seq_assembler_feed(a, &chunk)) {
            cwist_seq_assembler_destroy(a);
            return NULL;
        }
    }

    const uint8_t *data = NULL;
    size_t len = 0;
    bool ok = cwist_seq_assembler_get_data(a, &data, &len);
    char *token = NULL;
    if (ok && len > 0) {
        token = (char *)cwist_alloc(len + 1);
        if (token) {
            memcpy(token, data, len);
            token[len] = '\0';
        }
    }

    cwist_seq_assembler_destroy(a);
    return token;
}

/**
 * @brief Sign a payload as an HS256 JWT and split it into sequenced transport chunks.
 * @param payload_json Raw payload JSON string to sign.
 * @param secret Shared HS256 signing secret.
 * @param exp_seconds Lifetime in seconds to add when the payload lacks exp/iat.
 * @param chunk_payload_size Maximum payload bytes per chunk; must be non-zero.
 * @param out_count Set to the number of chunks produced.
 * @return Heap-allocated chunk array, or NULL when signing or chunking fails.
 *
 * Free the result with cwist_jwt_chunks_free().
 */
cwist_jwt_chunk_t *cwist_jwt_sign_chunks(const char *payload_json, const char *secret,
                                         long exp_seconds, uint16_t chunk_payload_size,
                                         size_t *out_count) {
    if (!payload_json || !secret || chunk_payload_size == 0 || !out_count) return NULL;
    *out_count = 0;

    char *token = cwist_jwt_sign(payload_json, secret, exp_seconds);
    if (!token) return NULL;

    cwist_jwt_chunk_t *chunks = cwist_jwt_split_chunks(token, chunk_payload_size, out_count);
    cwist_free(token);
    return chunks;
}

/**
 * @brief Reassemble sequenced chunks into a token, verify it, and return its claims.
 * @param chunks Array of sequenced chunks carrying the JWT token.
 * @param count Number of entries in @p chunks.
 * @param secret Shared HS256 signing secret.
 * @return Heap-allocated verified claims object, or NULL when assembly, parsing,
 *         signature verification, or claim validation fails.
 *
 * The caller owns the result and must release it with cwist_jwt_claims_destroy().
 */
cwist_jwt_claims *cwist_jwt_verify_chunks(const cwist_jwt_chunk_t *chunks, size_t count,
                                          const char *secret) {
    if (!chunks || count == 0 || !secret) return NULL;
    char *token = cwist_jwt_join_chunks(chunks, count);
    if (!token) return NULL;
    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    cwist_free(token);
    return claims;
}
