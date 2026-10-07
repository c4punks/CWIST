# HTTP 코어 API

*헤더:* `<cwist/net/http/http.h>`

저수준 HTTP 구조체, 프레이밍 한도, 파싱 로직입니다.

## 상수

모든 연결을 자원 고갈로부터 보호하기 위해 `<cwist/net/http/http.h>`에 정의되어 있습니다.

- `CWIST_HTTP_MAX_HEADER_SIZE` - 누적 헤더 바이트 상한 (기본 8KiB).
- `CWIST_HTTP_MAX_BODY_SIZE` - 메모리에 보관하는 POST 본문 바이트 상한 (기본 10MiB).
- `CWIST_HTTP_READ_BUFFER_SIZE` - 파이프라인/keep-alive 트래픽을 위한 연결별 임시 버퍼.
- `CWIST_HTTP_TIMEOUT_MS` - 헤더/본문을 기다리는 동안과 응답을 보내는 동안 쓰는 poll 타임아웃.

### `cwist_http_request_create`
```c
cwist_http_request *cwist_http_request_create(void);
```
새 HTTP 요청 구조체를 할당합니다.
`cwist_app_listen`은 들어오는 각 요청에 프레임워크 컨텍스트를 연결합니다.

- `req->app` - 실행 중인 `cwist_app`에 대한 역참조 (전역 설정을 가져올 때 유용).
- `req->db` - `cwist_app_use_db`로 설정한 공유 `cwist_db` 핸들.

### `cwist_http_parse_request`
```c
cwist_http_request *cwist_http_parse_request(const char *raw_request);
```
원시 HTTP 문자열을 `cwist_http_request` 객체로 파싱합니다.

### `cwist_http_receive_request`
```c
cwist_http_request *cwist_http_receive_request(
    int client_fd,
    char *read_buf,
    size_t buf_size,
    size_t *buf_len
);
```
keep-alive 소켓을 위한 블로킹, 프레이밍 읽기 도우미입니다.
- `CWIST_HTTP_MAX_HEADER_SIZE`를 지키면서 `\r\n\r\n`까지 읽습니다.
- 헤더와 `Content-Length`를 파싱하고, 본문 전체가 버퍼에 들어올 때까지 poll/`recv`를 계속합니다.
- *다음* 요청의 바이트는 `read_buf`에 남겨 두며, 호출자는 다음 반복에서 같은 버퍼를 다시 넘깁니다.
- 타임아웃, 잘못된 프레이밍, 한도 초과 시 `NULL`을 반환합니다.

### `cwist_http_response_create`
```c
cwist_http_response *cwist_http_response_create(void);
```
새 HTTP 응답 구조체를 할당합니다. 기본 상태는 200 OK입니다.

### `cwist_http_response_send_file`
```c
cwist_error_t cwist_http_response_send_file(
    cwist_http_response *res,
    const char *file_path,
    const char *content_type_hint,
    size_t *out_size
);
```
디스크의 정적 파일을 읽어 응답 본문에 복사하고 적절한 `Content-Type`을 설정합니다. `CWIST_HTTP_MAX_BODY_SIZE`보다 큰 파일은 `-EFBIG`로 거부하지만, 라우트에 `CWIST_ENDPOINT_FILE`이 붙어 있으면 Linux/macOS/FreeBSD에서 `sendfile(2)`로 전환해 커널이 직접 내용을 스트리밍합니다. 성공하면 `0`을 반환하고 실패하면 `-errno`(`-ENOENT`, `-EISDIR` 등)를 그대로 전달합니다. `out_size`는 선택 사항이며, 넘기면 파일 길이를 받으므로 핸들러가 페이로드를 버퍼에 담지 않고도 HEAD 응답을 만들 수 있습니다. 내장 테이블에는 `.wasm`, `.mjs` 같은 최신 웹 확장자도 들어 있습니다.

### `cwist_http_header_add`
```c
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value);
```
연결 리스트에 헤더를 추가합니다.

### `cwist_http_header_get`
```c
char *cwist_http_header_get(cwist_http_header_node *head, const char *key);
```
키로 헤더 값을 가져옵니다. 없으면 `NULL`을 반환합니다.
헤더 조회는 대소문자를 구분하지 않습니다 (`Host`와 `host`는 같습니다).

## 서버 코어

### Keep-alive 처리

`src/framework/app.c`의 `static_http_handler`는 이제 어느 한쪽이 `Connection: close`를 요청할 때까지 `cwist_http_receive_request`를 반복합니다. `cwist_http_send_response()`로 만든 모든 응답에는 명시적인 `Content-Length`가 들어가고, 전송 경로는 `poll()`+`send(MSG_NOSIGNAL)`을 사용하므로 ApacheBench가 keep-alive POST 64개를 동시에 쏟아붓는 동안에도 무한정 블로킹하거나 SIGPIPE를 일으키지 않습니다.

### 회귀 테스트

`tests/stress_test.c`는 최소한의 `/api` 엔드포인트를 띄우고 다음을 실행합니다.

```
ab -k -n 20000 -c 64 -p payload.json -T application/json http://127.0.0.1:31744/api
```

이 테스트는 서버가 각 요청 본문을 모두 읽고, 남은 바이트를 같은 소켓의 다음 요청에 재사용하고, `apr_pollset_poll timeout`이나 `connection reset` 없이 모든 응답을 마무리할 때만 성공합니다.

### `cwist_http_server_loop`
```c
cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx);
```
메인 서버 루프를 시작합니다.
- `config`로 반복형, 포크형, 멀티스레드 모델을 지원합니다.
- 스레드 안전한 상태 관리를 위해 `ctx`가 핸들러로 전달됩니다.

### `cwist_accept_socket`
```c
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int, void *), void *ctx);
```
저수준 accept 루프 래퍼입니다.

### `cwist_http_continuation_shed_count`
```c
long cwist_http_continuation_shed_count(void);
```
리액터 post 큐가 가득 차서 버려진(그리고 연결이 닫힌) 파이프라인 HTTP/1.1 continuation의 단조 증가 카운터입니다. Prometheus `/metrics`에서도 `cwist_http_continuation_shed_total`로 노출됩니다. 백프레셔와 버림 비율 경보에 유용합니다.

## 보안 헤더

### `cwist_http_response_add_security_headers`
```c
void cwist_http_response_add_security_headers(cwist_http_response *res);
```
기본 강화 헤더 세트를 각각 아직 없을 때만 응답에 넣습니다 (먼저 넣은 쪽이 이기므로, 핸들러가 헤더를 미리 설정하면 기본값을 덮어쓸 수 있습니다). HTTP와 HTTPS 응답 모두에서 안전하게 호출할 수 있습니다. `Strict-Transport-Security`는 설정하지 **않습니다**. 그 헤더는 `cwist_http_response_add_hsts()`로 넣으세요.

이 함수가 설정하는 헤더:

| 헤더 | 기본값 |
|---|---|
| `X-Frame-Options` | `DENY` |
| `X-Content-Type-Options` | `nosniff` |
| `Referrer-Policy` | `strict-origin-when-cross-origin` |
| `Content-Security-Policy` | 제한적인 default-src/script-src/style-src/font-src/img-src 정책 |
| `Cross-Origin-Resource-Policy` | `same-origin` |
| `Permissions-Policy` | camera/microphone/geolocation/payment/usb/interest-cohort 모두 거부 |
| `Cross-Origin-Opener-Policy` | `same-origin` |

### `cwist_http_response_add_hsts`
```c
void cwist_http_response_add_hsts(cwist_http_response *res);
```
TLS 응답에 `Strict-Transport-Security: max-age=31536000; includeSubDomains`를 추가합니다. 헤더가 이미 있으면 아무것도 하지 않습니다. RFC 6797 7.2절에 따라 HTTPS 핸들러에서만 호출하세요. 평문 HTTP 응답에는 절대 호출하면 안 됩니다.
