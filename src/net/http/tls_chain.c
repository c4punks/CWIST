#define _POSIX_C_SOURCE 200809L

#include "tls_chain.h"
#include "curl_global.h"

#include <curl/curl.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bytestring.h>
#include <openssl/err.h>
#include <openssl/nid.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CWIST_AIA_MAX_RESPONSE_BYTES (1024u * 1024u)
#define CWIST_AIA_MAX_CHAIN_DEPTH 8
#define CWIST_AIA_CONNECT_TIMEOUT_MS 2000L
#define CWIST_AIA_TIMEOUT_MS 5000L

typedef struct cwist_cert_fetch_buffer {
    uint8_t *data;
    size_t len;
    size_t cap;
} cwist_cert_fetch_buffer;

/**
 * @brief Check whether a URL uses the http or https scheme.
 *
 * Comparison is case-insensitive and matches only the scheme prefix.
 *
 * @param url URL string to inspect; may be NULL.
 * @return true if @p url starts with "http://" or "https://", false otherwise.
 */
static bool cwist_url_has_http_scheme(const char *url) {
    return url &&
           (strncasecmp(url, "http://", 7) == 0 ||
            strncasecmp(url, "https://", 8) == 0);
}

/**
 * @brief libcurl write callback that appends response bytes to a fetch buffer.
 *
 * Grows the buffer geometrically up to CWIST_AIA_MAX_RESPONSE_BYTES. Any
 * failure (size overflow, size cap exceeded, or realloc failure) aborts the
 * transfer by returning a short count.
 *
 * @param ptr incoming data bytes.
 * @param size size of each element.
 * @param nmemb number of elements.
 * @param userdata pointer to cwist_cert_fetch_buffer.
 * @return number of bytes consumed (size * nmemb) on success, 0 on failure.
 */
static size_t cwist_cert_fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;

    size_t total = size * nmemb;
    cwist_cert_fetch_buffer *buf = (cwist_cert_fetch_buffer *)userdata;
    if (total > CWIST_AIA_MAX_RESPONSE_BYTES || buf->len > CWIST_AIA_MAX_RESPONSE_BYTES - total) {
        return 0;
    }

    size_t need = buf->len + total;
    if (need > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < need) {
            if (new_cap > CWIST_AIA_MAX_RESPONSE_BYTES / 2) {
                new_cap = CWIST_AIA_MAX_RESPONSE_BYTES;
                break;
            }
            new_cap *= 2;
        }

        uint8_t *tmp = (uint8_t *)realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    return total;
}

/**
 * @brief Fetch a certificate (or certificate container) over HTTP(S).
 *
 * Only http/https URLs are accepted. The curl global state is acquired for
 * the duration of the request. Timeouts and redirect limits are bounded by
 * CWIST_AIA_* constants. On success @p out owns a malloc'd buffer that the
 * caller must free; on failure @p out is zeroed and holds nothing.
 *
 * @param url http or https URL to fetch.
 * @param out fetch buffer to fill; must not be NULL.
 * @return true on success, false on failure.
 */
static bool cwist_fetch_cert_url(const char *url, cwist_cert_fetch_buffer *out) {
    if (!cwist_url_has_http_scheme(url) || !out) return false;

    cwist_curl_global_acquire();
    CURL *curl = curl_easy_init();
    if (!curl) {
        cwist_curl_global_release();
        return false;
    }

    memset(out, 0, sizeof(*out));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CWIST_AIA_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, CWIST_AIA_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CWIST-TLS-AIA/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cwist_cert_fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
#if defined(CURLOPT_PROTOCOLS_STR)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_PROTOCOLS)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    cwist_curl_global_release();

    if (code != CURLE_OK || out->len == 0) {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return false;
    }

    return true;
}

/**
 * @brief Parse a single DER-encoded X.509 certificate.
 *
 * OpenSSL error state is cleared before returning NULL on parse failure.
 *
 * @param data DER certificate bytes.
 * @param len length of @p data.
 * @return newly allocated stack containing one X509 (caller frees), or NULL.
 */
static STACK_OF(X509) * cwist_parse_der_x509(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > LONG_MAX) return NULL;

    const uint8_t *p = data;
    X509 *cert = d2i_X509(NULL, &p, (long)len);
    if (!cert) {
        ERR_clear_error();
        return NULL;
    }

    STACK_OF(X509) *certs = sk_X509_new_null();
    if (!certs || !sk_X509_push(certs, cert)) {
        X509_free(cert);
        sk_X509_free(certs);
        return NULL;
    }
    return certs;
}

/**
 * @brief Parse PEM-encoded X.509 certificates.
 *
 * Reads every PEM certificate from the buffer. OpenSSL error state is
 * cleared before returning.
 *
 * @param data PEM certificate text.
 * @param len length of @p data.
 * @return newly allocated stack of X509 (caller frees), or NULL if none
 *         could be parsed.
 */
static STACK_OF(X509) * cwist_parse_pem_x509(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > INT_MAX) return NULL;

    BIO *bio = BIO_new_mem_buf(data, (int)len);
    if (!bio) return NULL;

    STACK_OF(X509) *certs = sk_X509_new_null();
    if (!certs) {
        BIO_free(bio);
        return NULL;
    }

    X509 *cert = NULL;
    while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (!sk_X509_push(certs, cert)) {
            X509_free(cert);
            sk_X509_pop_free(certs, X509_free);
            BIO_free(bio);
            return NULL;
        }
    }
    ERR_clear_error();
    BIO_free(bio);

    if (sk_X509_num(certs) == 0) {
        sk_X509_free(certs);
        return NULL;
    }
    return certs;
}

/**
 * @brief Parse a DER-encoded PKCS#7 container and extract its certificates.
 *
 * OpenSSL error state is cleared before returning NULL on failure.
 *
 * @param data DER PKCS#7 bytes.
 * @param len length of @p data.
 * @return newly allocated stack of X509 (caller frees), or NULL.
 */
static STACK_OF(X509) * cwist_parse_der_pkcs7(const uint8_t *data, size_t len) {
    if (!data || len == 0) return NULL;

    STACK_OF(X509) *certs = sk_X509_new_null();
    if (!certs) return NULL;

    CBS cbs;
    CBS_init(&cbs, data, len);
    if (!PKCS7_get_certificates(certs, &cbs) || sk_X509_num(certs) == 0) {
        ERR_clear_error();
        sk_X509_pop_free(certs, X509_free);
        return NULL;
    }

    return certs;
}

/**
 * @brief Parse a PEM-encoded PKCS#7 container and extract its certificates.
 *
 * OpenSSL error state is cleared before returning NULL on failure.
 *
 * @param data PEM PKCS#7 text.
 * @param len length of @p data.
 * @return newly allocated stack of X509 (caller frees), or NULL.
 */
static STACK_OF(X509) * cwist_parse_pem_pkcs7(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > INT_MAX) return NULL;

    BIO *bio = BIO_new_mem_buf(data, (int)len);
    if (!bio) return NULL;

    STACK_OF(X509) *certs = sk_X509_new_null();
    if (!certs) {
        BIO_free(bio);
        return NULL;
    }

    if (!PKCS7_get_PEM_certificates(certs, bio) || sk_X509_num(certs) == 0) {
        ERR_clear_error();
        sk_X509_pop_free(certs, X509_free);
        BIO_free(bio);
        return NULL;
    }

    BIO_free(bio);
    return certs;
}

/**
 * @brief Try each supported certificate container format in turn.
 *
 * Attempts, in order: DER X.509, PEM X.509, DER PKCS#7, PEM PKCS#7.
 *
 * @param data raw certificate bytes of unknown encoding.
 * @param len length of @p data.
 * @return newly allocated stack of X509 from the first format that parses
 *         (caller frees), or NULL if none matched.
 */
static STACK_OF(X509) * cwist_parse_certificates(const uint8_t *data, size_t len) {
    STACK_OF(X509) *certs = cwist_parse_der_x509(data, len);
    if (certs) return certs;

    certs = cwist_parse_pem_x509(data, len);
    if (certs) return certs;

    certs = cwist_parse_der_pkcs7(data, len);
    if (certs) return certs;

    return cwist_parse_pem_pkcs7(data, len);
}

/**
 * @brief Check whether @p issuer is a valid issuing CA for @p subject.
 *
 * Rejects identical certificates, non-CA issuers, name/constraint
 * mismatches, and signatures that do not verify with the issuer's public
 * key. No reference counts are changed.
 *
 * @param issuer candidate issuer certificate.
 * @param subject candidate subject (child) certificate.
 * @return true if @p issuer validly signed @p subject, false otherwise.
 */
static bool cwist_x509_is_valid_issuer(X509 *issuer, X509 *subject) {
    if (!issuer || !subject || X509_cmp(issuer, subject) == 0) return false;
    if (X509_check_ca(issuer) != 1) return false;
    if (X509_check_issued(issuer, subject) != X509_V_OK) return false;

    EVP_PKEY *issuer_key = X509_get0_pubkey(issuer);
    return issuer_key && X509_verify(subject, issuer_key) == 1;
}

/**
 * @brief Find the first certificate in @p candidates that validly issued
 *        @p subject.
 *
 * The returned pointer is borrowed from @p candidates; no reference count
 * is incremented.
 *
 * @param subject certificate whose issuer is sought.
 * @param candidates stack of candidate issuer certificates.
 * @return matching issuer certificate, or NULL if none qualifies.
 */
static X509 *cwist_find_issuer(X509 *subject, STACK_OF(X509) * candidates) {
    if (!subject || !candidates) return NULL;

    for (size_t i = 0; i < sk_X509_num(candidates); i++) {
        X509 *candidate = sk_X509_value(candidates, i);
        if (cwist_x509_is_valid_issuer(candidate, subject)) {
            return candidate;
        }
    }
    return NULL;
}

/**
 * @brief Check whether @p cert is already present in the SSL_CTX chain.
 *
 * Compares against both the leaf certificate and the extra chain
 * certificates. Returned pointers are borrowed; no reference counts change.
 *
 * @param ssl_ctx SSL context to inspect.
 * @param cert certificate to look for.
 * @return true if an identical certificate is already installed, false
 *         otherwise.
 */
static bool cwist_ctx_contains_cert(SSL_CTX *ssl_ctx, X509 *cert) {
    if (!ssl_ctx || !cert) return false;

    X509 *leaf = SSL_CTX_get0_certificate(ssl_ctx);
    if (leaf && X509_cmp(leaf, cert) == 0) return true;

    STACK_OF(X509) *chain = NULL;
    if (SSL_CTX_get0_chain_certs(ssl_ctx, &chain) == 1 && chain) {
        for (size_t i = 0; i < sk_X509_num(chain); i++) {
            if (X509_cmp(sk_X509_value(chain, i), cert) == 0) return true;
        }
    }
    return false;
}

/**
 * @brief Return the last certificate in the SSL_CTX chain.
 *
 * The returned pointer is borrowed from the SSL_CTX; no reference count
 * is incremented. If no extra chain certificates are set, the leaf
 * certificate is returned.
 *
 * @param ssl_ctx SSL context to inspect.
 * @return chain-tail certificate, or NULL if @p ssl_ctx has no certificate.
 */
static X509 *cwist_ctx_chain_tail(SSL_CTX *ssl_ctx) {
    if (!ssl_ctx) return NULL;

    STACK_OF(X509) *chain = NULL;
    if (SSL_CTX_get0_chain_certs(ssl_ctx, &chain) == 1 && chain && sk_X509_num(chain) > 0) {
        return sk_X509_value(chain, sk_X509_num(chain) - 1);
    }
    return SSL_CTX_get0_certificate(ssl_ctx);
}

/**
 * @brief Check whether a certificate is self-signed (self-issued and its
 *        signature verifies with its own public key).
 *
 * @param cert certificate to test.
 * @return true if @p cert is NULL (treated as end of chain) or self-signed,
 *         false otherwise.
 */
static bool cwist_x509_is_self_signed(X509 *cert) {
    if (!cert) return true;
    if (X509_NAME_cmp(X509_get_subject_name(cert), X509_get_issuer_name(cert)) != 0) {
        return false;
    }

    EVP_PKEY *key = X509_get0_pubkey(cert);
    return key && X509_verify(cert, key) == 1;
}

/**
 * @brief Extract an http/https caIssuers URI from an ACCESS_DESCRIPTION.
 *
 * Only ad_caIssuers entries whose location is a GEN_URI without embedded
 * NUL bytes and with an http/https scheme are accepted.
 *
 * @param desc access description from a certificate's AIA extension.
 * @return newly allocated NUL-terminated URL string (caller frees), or
 *         NULL if the entry is not a usable http/https URI.
 */
static char *cwist_dup_aia_uri(const ACCESS_DESCRIPTION *desc) {
    if (!desc || OBJ_obj2nid(desc->method) != NID_ad_ca_issuers || !desc->location) {
        return NULL;
    }

    int name_type = -1;
    ASN1_IA5STRING *uri = (ASN1_IA5STRING *)GENERAL_NAME_get0_value(desc->location, &name_type);
    if (name_type != GEN_URI || !uri) return NULL;

    int uri_len = ASN1_STRING_length(uri);
    if (uri_len <= 0) return NULL;

    const unsigned char *uri_data = ASN1_STRING_get0_data(uri);
    if (!uri_data || memchr(uri_data, '\0', (size_t)uri_len)) return NULL;

    char *url = (char *)malloc((size_t)uri_len + 1);
    if (!url) return NULL;
    memcpy(url, uri_data, (size_t)uri_len);
    url[uri_len] = '\0';

    if (!cwist_url_has_http_scheme(url)) {
        free(url);
        return NULL;
    }
    return url;
}

/**
 * @brief Fetch a certificate container from @p url and return an issuer of
 *        @p subject found within it.
 *
 * Fetches the URL, parses it in every supported format, and selects a
 * valid issuing certificate. On success the returned X509 owns one new
 * reference that the caller must free.
 *
 * @param url http/https URL to fetch.
 * @param subject certificate whose issuer is sought.
 * @return newly referenced issuer certificate (caller frees), or NULL.
 */
static X509 *cwist_fetch_issuer_from_url(const char *url, X509 *subject) {
    cwist_cert_fetch_buffer buf = {0};
    if (!cwist_fetch_cert_url(url, &buf)) return NULL;

    STACK_OF(X509) *certs = cwist_parse_certificates(buf.data, buf.len);
    free(buf.data);
    if (!certs) return NULL;

    X509 *issuer = cwist_find_issuer(subject, certs);
    if (issuer && X509_up_ref(issuer) != 1) {
        issuer = NULL;
    }

    sk_X509_pop_free(certs, X509_free);
    return issuer;
}

/**
 * @brief Fetch an issuer certificate via the subject's AIA caIssuers URLs.
 *
 * Iterates the subject's Authority Information Access extension and tries
 * each http/https caIssuers URI in order until a valid issuer is found.
 * OpenSSL error state is cleared if no AIA extension is present. On
 * success the returned X509 owns one new reference that the caller must
 * free.
 *
 * @param subject certificate whose AIA extension should be consulted.
 * @return newly referenced issuer certificate (caller frees), or NULL.
 */
static X509 *cwist_fetch_issuer_from_aia(X509 *subject) {
    if (!subject) return NULL;

    AUTHORITY_INFO_ACCESS *aia =
        (AUTHORITY_INFO_ACCESS *)X509_get_ext_d2i(subject, NID_info_access, NULL, NULL);
    if (!aia) {
        ERR_clear_error();
        return NULL;
    }

    X509 *issuer = NULL;
    for (size_t i = 0; i < sk_ACCESS_DESCRIPTION_num(aia); i++) {
        ACCESS_DESCRIPTION *desc = sk_ACCESS_DESCRIPTION_value(aia, i);
        char *url = cwist_dup_aia_uri(desc);
        if (!url) continue;

        issuer = cwist_fetch_issuer_from_url(url, subject);
        free(url);
        if (issuer) break;
    }

    AUTHORITY_INFO_ACCESS_free(aia);
    return issuer;
}

/**
 * @brief Autoload missing intermediate CA certificates into an SSL_CTX chain.
 *
 * Starting from the current chain tail, walks up the issuance path using AIA
 * caIssuers URLs until a self-signed (root) certificate is reached, the chain
 * depth limit (CWIST_AIA_MAX_CHAIN_DEPTH) is hit, or no issuer can be
 * fetched. Issuers that would duplicate a certificate already installed are
 * not added.
 *
 * @param ssl_ctx SSL context whose certificate chain should be completed.
 * @return number of intermediate certificates added (0 if none were needed or
 *         found), or -1 on error.
 */
int cwist_tls_autoload_intermediates(SSL_CTX *ssl_ctx) {
    if (!ssl_ctx) return -1;

    int added = 0;
    for (int depth = 0; depth < CWIST_AIA_MAX_CHAIN_DEPTH; depth++) {
        X509 *tail = cwist_ctx_chain_tail(ssl_ctx);
        if (!tail || cwist_x509_is_self_signed(tail)) break;

        X509 *issuer = cwist_fetch_issuer_from_aia(tail);
        if (!issuer) break;

        if (cwist_x509_is_self_signed(issuer)) {
            X509_free(issuer);
            break;
        }

        if (cwist_ctx_contains_cert(ssl_ctx, issuer)) {
            X509_free(issuer);
            break;
        }

        if (SSL_CTX_add1_chain_cert(ssl_ctx, issuer) != 1) {
            X509_free(issuer);
            return -1;
        }

        X509_free(issuer);
        added++;
    }

    return added;
}
