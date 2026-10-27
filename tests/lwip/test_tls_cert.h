/* TEST-ONLY credentials for the lwIP TLS tests: a self-signed ECDSA P-256
 * certificate for CN/SAN "localhost" (also its own CA) and its private key.
 * The key is public in this repository; never deploy it.
 * Generated with:
 *   openssl ecparam -name prime256v1 -genkey -noout -out key.pem
 *   openssl req -new -x509 -key key.pem -days 36500 -subj "/CN=localhost" \
 *       -addext "subjectAltName=DNS:localhost" -addext "basicConstraints=critical,CA:TRUE"
 *   openssl pkey -in key.pem    (PKCS#8)
 * PEM input to mbedTLS must include the terminating NUL in its length, so use
 * sizeof(), not strlen(). */
#ifndef CWIST_TEST_TLS_CERT_H
#define CWIST_TEST_TLS_CERT_H

static const char CWIST_TEST_TLS_CERT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBdTCCARqgAwIBAgIUDlz574UUF7qlFt+BIr6riP7RPS0wCgYIKoZIzj0EAwIw\n"
    "FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAyNzAyMTI1MFoYDzIxMjYxMDAz\n"
    "MDIxMjUwWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
    "PQMBBwNCAASFRd0A1JRBAxzv1L515l339DD+rnew/9bmNNwRjg8cF44knagwZh38\n"
    "SwHGK1HQe/ctpIJV0e5rAUiPvaPf4hpAo0gwRjAdBgNVHQ4EFgQUjIeSkEK5DHYH\n"
    "fqfmshNlBYqbdFQwFAYDVR0RBA0wC4IJbG9jYWxob3N0MA8GA1UdEwEB/wQFMAMB\n"
    "Af8wCgYIKoZIzj0EAwIDSQAwRgIhAPnGuFOrFqQ7W0WUrfIbU49m940P9abpVTvW\n"
    "skFuIG8cAiEAltt3dRS2Jr8vqDfG6U8zDkpAVfWAOiVeaQFxMV9v6E0=\n"
    "-----END CERTIFICATE-----\n";

static const char CWIST_TEST_TLS_KEY_PEM[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAtGZQPkk+DDMBHB8\n"
    "wUmP+Y5TzD+opvlnfiATjaiehb2hRANCAASFRd0A1JRBAxzv1L515l339DD+rnew\n"
    "/9bmNNwRjg8cF44knagwZh38SwHGK1HQe/ctpIJV0e5rAUiPvaPf4hpA\n"
    "-----END PRIVATE KEY-----\n";

/* A second, unrelated P-256 key: must be rejected as not matching the certificate. */
static const char CWIST_TEST_TLS_OTHER_KEY_PEM[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgks12p7K6TkaiDCK4\n"
    "uq1AdKjqLdEClsDLFn2J8MdPkpuhRANCAARuKhKOwcMQGJpW/g+kBGF1UXuboDiN\n"
    "7hns+KuWE5TM1yGf+CP96pj1bSRzmqz0l/kfXoL9sCKjfSOcheOapoQ0\n"
    "-----END PRIVATE KEY-----\n";

#endif /* CWIST_TEST_TLS_CERT_H */
