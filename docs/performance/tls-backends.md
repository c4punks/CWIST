# Pluggable TLS backend (CWIST_TLS)

CWIST links exactly one TLS/crypto backend, selected at build time:

```sh
make                          # CWIST_TLS=boringssl (default, unchanged)
make CWIST_TLS=mbedtls        # mbedTLS 3.6 LTS (vendored at lib/mbedtls)
```

`CWIST_TLS=mbedtls` is meant for embedded targets: mbedTLS 3.6 is a pure-C
LTS release with a fraction of BoringSSL's archive footprint, and the
produced `libcwist.a` (thin or fat) contains **no BoringSSL objects or
symbols**.

## The seam

Call sites keep their OpenSSL-shaped code.  When `CWIST_TLS=mbedtls` the
Makefile puts `src/security/tls/shim/` on the include path instead of
`lib/boringssl/include`, so `<openssl/ssl.h>` and friends resolve to the shim
headers in `src/security/tls/shim/openssl/`.  Each `SSL_*`/`HMAC_*`/`EVP_*`
name is a macro alias for a `cwist_mtls_*` function genuinely implemented on
mbedTLS in `src/security/tls/backend_mbedtls.c`.  Public headers are
unchanged (`SSL`/`SSL_CTX` become opaque typedefs), so the API baseline and
existing applications are untouched.

Covered by the seam: server and client TLS 1.2/1.3, certificate chain and key
loading, ALPN (HTTP/2), SNI, the session cache, pre-fork shared session
tickets (armed from the same CWIST key material, so tickets survive
`cwist_https_reload_context()` and resume on any forked worker), certificate
hot reload, error mapping (`SSL_get_error`/`ERR_*`), HMAC-SHA256 (sessions,
seq_auth, JWT HS256), AES-256-CBC (db_crypt, format-compatible with the
BoringSSL build), and a fork-aware CTR-DRBG behind `RAND_bytes`.

## Forced-off features under CWIST_TLS=mbedtls

| Feature | Why | Behaviour |
| --- | --- | --- |
| HTTP/3 (`CWIST_HTTP3`) | lsquic needs BoringSSL's QUIC API | forced to 0 with a `$(warning)` |
| WebTransport | rides on HTTP/3 | forced to `none` |
| WebRTC DataChannel (`CWIST_WEBRTC`) | DTLS stack is BoringSSL-specific | forced to 0 with a `$(warning)` |

## Degraded features under CWIST_TLS=mbedtls

| Feature | Behaviour |
| --- | --- |
| JWT EdDSA (Ed25519) | sign/verify fail at runtime; HS256 and ES256 work (mbedTLS 3.6 has no Ed25519) |
| Session-resumption counter | `cwist_https_tls_handshakes_resumed_total()` stays 0 (no public per-connection flag in mbedTLS) |
| AIA intermediate autoload (`tls_chain.c`) | no-op; serve a PEM that already contains the intermediates |
| ECH (`cwist_app_use_ech`) | no-op (needs BoringSSL/OpenSSL 3.2+) |
| Hybrid PQC groups (`cwist_app_use_pqc_layer`) | warns and enforces TLS 1.3 with classical groups (no X25519MLKEM768 in mbedTLS 3.6) |

## Embedded bundle

`make CWIST_TLS=mbedtls CWIST_HTTP3=0 fat-dist` stages
`dist/cwist-fat/include/openssl/` from the seam shim (plus `mbedtls/` and
`psa/` for direct mbedTLS use) instead of BoringSSL's headers, so an app
source that builds against the BoringSSL bundle builds against the mbedTLS
bundle unchanged.
