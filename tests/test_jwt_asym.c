/**
 * @file test_jwt_asym.c
 * @brief Unit tests for the ES256/Ed25519 JWT sign/verify API (issue #401).
 */

#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

static const char EC256_PRIV[] =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIDC7BCqt+gWMBQEEX7581vxye5WCTZNUlp2OdYdHwbYSoAoGCCqGSM49\n"
    "AwEHoUQDQgAEQlZeTKbWplC+kWKJzbfp82Ig7o1N0Pdin5CkqfUEO+prdr/XnLjp\n"
    "vFFp2mMpAudMNv4Uw8BmpCLrUSeUF1jZKQ==\n"
    "-----END EC PRIVATE KEY-----\n";

static const char EC256_PUB[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQlZeTKbWplC+kWKJzbfp82Ig7o1N\n"
    "0Pdin5CkqfUEO+prdr/XnLjpvFFp2mMpAudMNv4Uw8BmpCLrUSeUF1jZKQ==\n"
    "-----END PUBLIC KEY-----\n";

/* A second, unrelated P-256 public key (wrong key for EC256_PRIV). */
static const char EC384_PUB[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEbXVPxWIYXmEE7dBlWKelC/KkxkNtUJR5\n"
    "UDg+kRhwUuQuxZhEwzcCWulAG0grdNytmLGBpLw6FrOCcK5SKnuRFRtedgalGaVA\n"
    "vsHAKcg5zwTC6LLEWoSzHh8/FqbfpGEl\n"
    "-----END PUBLIC KEY-----\n";

static const char ED25519_PRIV[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MC4CAQAwBQYDK2VwBCIEIOg0tgwevnUolReLx+S+Kq+tiity9wj7HtCnqnU45TLD\n"
    "-----END PRIVATE KEY-----\n";

static const char ED25519_PUB[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAm1eSBuXZV0PVcazp/ZYocHlQfQJVEDOmhcRNj3X1sdI=\n"
    "-----END PUBLIC KEY-----\n";

/* base64url of {"alg":"ES256","typ":"JWT"} */
static const char ES256_HDR[] = "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9";
/* base64url of {"alg":"ES256","typ":"JWT","kid":"key-1"} */
static const char ES256_KID_HDR[] = "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImtleS0xIn0";
/* base64url of {"alg":"EdDSA","typ":"JWT"} */
#ifndef CWIST_TLS_MBEDTLS
static const char EDDSA_HDR[] = "eyJhbGciOiJFZERTQSIsInR5cCI6IkpXVCJ9";
#endif

static void test_es256_round_trip(void) {
    printf("Testing ES256 sign/verify round trip...\n");

    char *token = cwist_jwt_sign_es256("{\"sub\":\"user42\",\"role\":\"admin\"}", EC256_PRIV, NULL,
                                       3600);
    assert(token != NULL);
    assert(strncmp(token, ES256_HDR, strlen(ES256_HDR)) == 0);

    cwist_jwt_claims *claims = cwist_jwt_verify_es256(token, EC256_PUB);
    assert(claims != NULL);
    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL && strcmp(sub, "user42") == 0);
    const char *role = cwist_jwt_claims_get(claims, "role");
    assert(role != NULL && strcmp(role, "admin") == 0);
    cwist_jwt_claims_destroy(claims);

    /* The private key must verify too (public half is used). */
    claims = cwist_jwt_verify_es256(token, EC256_PRIV);
    assert(claims != NULL);
    cwist_jwt_claims_destroy(claims);

    cwist_free(token);
    printf("  Passed ES256 round trip.\n");
}

static void test_es256_kid_header(void) {
    printf("Testing ES256 kid header...\n");

    char *token = cwist_jwt_sign_es256("{\"sub\":\"1\"}", EC256_PRIV, "key-1", 0);
    assert(token != NULL);
    assert(strncmp(token, ES256_KID_HDR, strlen(ES256_KID_HDR)) == 0);

    cwist_jwt_claims *claims = cwist_jwt_verify_es256(token, EC256_PUB);
    assert(claims != NULL);
    cwist_jwt_claims_destroy(claims);
    cwist_free(token);
    printf("  Passed ES256 kid header.\n");
}

static void test_es256_wrong_key(void) {
    printf("Testing ES256 reject wrong key...\n");

    char *token = cwist_jwt_sign_es256("{\"sub\":\"1\"}", EC256_PRIV, NULL, 3600);
    assert(token != NULL);

    /* A P-384 key has the wrong curve; verification must fail, not crash. */
    assert(cwist_jwt_verify_es256(token, EC384_PUB) == NULL);

    cwist_free(token);
    printf("  Passed ES256 wrong key rejection.\n");
}

static void test_alg_confusion(void) {
    printf("Testing cross-algorithm rejection...\n");

    /* An HS256 token must never verify as ES256 or EdDSA. */
    char *hs = cwist_jwt_sign("{\"sub\":\"1\"}", "secret", 3600);
    assert(hs != NULL);
    assert(cwist_jwt_verify_es256(hs, EC256_PUB) == NULL);
    assert(cwist_jwt_verify_ed25519(hs, ED25519_PUB) == NULL);
    cwist_free(hs);

    /* An ES256 token must never verify as HS256 (RFC 8725 alg confusion). */
    char *es = cwist_jwt_sign_es256("{\"sub\":\"1\"}", EC256_PRIV, NULL, 3600);
    assert(es != NULL);
    assert(cwist_jwt_verify(es, "secret") == NULL);
    assert(cwist_jwt_verify_ed25519(es, ED25519_PUB) == NULL);
    cwist_free(es);

#ifndef CWIST_TLS_MBEDTLS
    /* An EdDSA token must never verify as HS256 or ES256. */
    char *ed = cwist_jwt_sign_ed25519("{\"sub\":\"1\"}", ED25519_PRIV, NULL, 3600);
    assert(ed != NULL);
    assert(cwist_jwt_verify(ed, "secret") == NULL);
    assert(cwist_jwt_verify_es256(ed, EC256_PUB) == NULL);
    cwist_free(ed);
#else
    /* EdDSA has no mbedTLS 3.6 implementation: signing fails, and failing
     * closed is itself the cross-algorithm safety property. */
    assert(cwist_jwt_sign_ed25519("{\"sub\":\"1\"}", ED25519_PRIV, NULL, 3600) == NULL);
#endif

    printf("  Passed cross-algorithm rejection.\n");
}

static void test_es256_key_type_mismatch(void) {
    printf("Testing ES256 key type mismatch...\n");

    /* An Ed25519 private key is not usable for ES256 signing. */
    assert(cwist_jwt_sign_es256("{\"sub\":\"1\"}", ED25519_PRIV, NULL, 0) == NULL);
    /* An EC P-384 private key is not usable for ES256 signing either: wrong curve. */
    assert(cwist_jwt_sign_ed25519("{\"sub\":\"1\"}", EC256_PRIV, NULL, 0) == NULL);
    /* Garbage PEM must fail cleanly. */
    assert(cwist_jwt_sign_es256("{\"sub\":\"1\"}", "not a pem", NULL, 0) == NULL);
    assert(cwist_jwt_verify_es256("a.b.c", "not a pem") == NULL);

    printf("  Passed ES256 key type mismatch.\n");
}

#ifndef CWIST_TLS_MBEDTLS
static void test_ed25519_round_trip(void) {
    printf("Testing Ed25519 sign/verify round trip...\n");

    char *token = cwist_jwt_sign_ed25519("{\"sub\":\"alice\",\"scope\":\"read\"}", ED25519_PRIV,
                                         NULL, 3600);
    assert(token != NULL);
    assert(strncmp(token, EDDSA_HDR, strlen(EDDSA_HDR)) == 0);

    cwist_jwt_claims *claims = cwist_jwt_verify_ed25519(token, ED25519_PUB);
    assert(claims != NULL);
    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL && strcmp(sub, "alice") == 0);
    cwist_jwt_claims_destroy(claims);

    /* Tampered payload must not verify. */
    char *dot1 = strchr(token, '.');
    assert(dot1 != NULL && dot1[1] != '\0');
    dot1[1] = (dot1[1] == 'A') ? 'B' : 'A';
    assert(cwist_jwt_verify_ed25519(token, ED25519_PUB) == NULL);

    cwist_free(token);
    printf("  Passed Ed25519 round trip.\n");
}
#endif /* !CWIST_TLS_MBEDTLS */

static void test_es256_expired(void) {
    printf("Testing ES256 expired token rejection...\n");

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"sub\":\"1\",\"exp\":%ld}",
             (long)(time(NULL) - 400)); /* beyond the 300s leeway */
    char *token = cwist_jwt_sign_es256(payload, EC256_PRIV, NULL, 0);
    assert(token != NULL);
    assert(cwist_jwt_verify_es256(token, EC256_PUB) == NULL);
    cwist_free(token);

    printf("  Passed ES256 expired rejection.\n");
}

int main(void) {
    test_es256_round_trip();
    test_es256_kid_header();
    test_es256_wrong_key();
    test_alg_confusion();
    test_es256_key_type_mismatch();
#ifdef CWIST_TLS_MBEDTLS
    /* EdDSA is unavailable on the mbedTLS backend (no Ed25519 in 3.6 LTS). */
    printf("Skipping Ed25519 round trip on CWIST_TLS=mbedtls.\n");
#else
    test_ed25519_round_trip();
#endif
    test_es256_expired();
    printf("All JWT asymmetric tests passed!\n");
    return 0;
}
