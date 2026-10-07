# HTTPS API

*헤더:* `<cwist/net/http/https.h>`

선택적인 포스트 양자 암호(PQC)를 지원하는 BoringSSL 기반 보안 전송 계층입니다.

### `cwist_https_init_context`
```c
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path);
```
TLS 컨텍스트를 초기화하고 인증서를 불러옵니다.
컨텍스트는 TLS 1.3 이상을 강제하고 TLS 수준 압축을 비활성화합니다.
`cwist_app_use_pqc_layer()`로 PQC 계층을 켜면, 컨텍스트는 키 교환을 하이브리드 그룹 `X25519MLKEM768:X25519:P-256`으로 추가 제한합니다. NIST 표준 ML-KEM-768(Kyber)과 고전적인 X25519 ECDH를 결합해 양자 내성 전송을 제공합니다.

### `cwist_https_init_context_with_options`
```c
cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options);
```
명시적인 전송 옵션으로 TLS 컨텍스트를 만듭니다.
`options->enable_http2`가 참이면 CWIST는 ALPN 협상과 더 엄격한 암호 선호 같은 HTTP/2 지향 TLS 기본값을 기존 HTTPS 설정에 합칩니다.

### `cwist_https_accept`
```c
cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn);
```
accept한 TCP 소켓에서 SSL 핸드셰이크를 수행합니다.

### `cwist_https_send_response`
```c
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);
```
응답을 직렬화하고 암호화해 클라이언트로 보냅니다.

### `cwist_https_connection_uses_http2`
```c
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn);
```
TLS 연결에서 ALPN이 `h2`를 협상했는지 반환합니다.

### `cwist_http2_serve_connection`
```c
cwist_error_t cwist_http2_serve_connection(cwist_https_connection *conn,
                                           void *user_ctx,
                                           cwist_http2_request_handler_func handler);
```
HTTP/2 클라이언트 preface와 프레임 스트림을 받아 요청 헤더/본문을 디코딩하고, 앱 핸들러를 호출한 뒤, 응답을 HTTP/2 `HEADERS`/`DATA` 프레임으로 내보냅니다.
