# 프레임워크와 앱 API

*헤더:* `<cwist/sys/app/app.h>` (제안)

웹 애플리케이션을 빠르게 만들기 위한 고수준 추상화입니다.

### `cwist_app_create`
```c
cwist_app *cwist_app_create(void);
```
기본 보안 설정으로 새 웹 애플리케이션 인스턴스를 초기화합니다.

### `cwist_app_use_https`
```c
cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);
```
애플리케이션에서 HTTPS를 켭니다.

### `cwist_app_use_https2` / `cwist_use_https2`
```c
cwist_error_t cwist_app_use_https2(cwist_app *app, bool enabled);

// 앱 변수 이름이 `app`일 때 쓰는 편의 매크로
cwist_use_https2(true);
```
명시적으로 켜기 전까지 기본 HTTPS 요청 경로는 기존 `HTTP/1.1` 모드로 유지됩니다.
켜면 CWIST는 HTTP/2와 호환되는 TLS 프로필로 TLS 컨텍스트를 다시 만들고, ALPN으로 `h2`를 협상하며, 앱의 HTTPS 요청 핸들러 슬롯을 HTTPS/2용으로 바꿉니다.
HTTP/2 핸들러는 디코딩한 요청을 기존 라우팅/미들웨어 스택으로 넘기므로 핸들러 코드를 바꿀 필요가 없습니다.
현재 구현은 의도적으로 범위가 좁습니다. TLS 연결 하나당 한 번에 요청 스트림 하나를 처리하며, 표준 클라이언트 preface, `SETTINGS`, `HEADERS`, `CONTINUATION`, `DATA`, `PING`, `GOAWAY`를 지원합니다.

### `cwist_app_use_pqc_layer`
```c
void cwist_app_use_pqc_layer(cwist_app *app, bool enabled);
```
포스트 양자 암호(PQC) 하이브리드 TLS 계층을 켭니다.
`enabled`가 `true`이면 CWIST는 키 교환 그룹 목록을 `X25519MLKEM768:X25519:P-256`으로 강제하고, 최소 버전을 TLS 1.3으로 설정하며, 기존 TLSv1.0-1.2 암호 스위트를 모두 비활성화합니다.
NIST 표준 ML-KEM-768(Kyber)과 고전적인 X25519 ECDH를 결합한 하이브리드 방식으로, 애플리케이션이 OpenSSL을 몰라도 **전송 계층의 양자 내성**을 얻습니다.
애플리케이션 코드는 BoringSSL을 직접 다루지 않습니다.

### `cwist_app_use_db`
```c
cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path);
cwist_db *cwist_app_get_db(cwist_app *app);
```
SQLite 데이터베이스를 열고(또는 다시 열고) 핸들을 `cwist_app`에 보관합니다. 들어오는 모든 요청은 `req->db`로 이 포인터를 자동으로 받으므로, 핸들러는 전역 변수 없이 쿼리를 실행할 수 있습니다.
```c
void profile_handler(cwist_http_request *req, cwist_http_response *res) {
    cJSON *rows = NULL;
    if (cwist_db_query(req->db, "SELECT name FROM profiles LIMIT 1", &rows).error.err_i16 == 0) {
        char *json = cJSON_PrintUnformatted(rows);
        cwist_sstring_assign(res->body, json);
        free(json);
        cJSON_Delete(rows);
    }
}
```

## 라우팅

### `cwist_app_get` / `cwist_app_post`
표준 HTTP 핸들러를 등록합니다. `:name` 문법으로 경로 파라미터를 지원합니다.

```c
void user_handler(cwist_http_request *req, cwist_http_response *res) {
    char *user_id = cwist_query_map_get(req->path_params, "id");
    // ...
}

cwist_app_get(app, "/users/:id", user_handler);
```

### `cwist_app_get_ex` / `_post_ex` / `_put_ex` / `_delete_ex` / `_patch_ex`
핸들러가 컨텍스트 포인터도 함께 받는 라우트를 등록합니다. 덕분에 전역 변수 없이 상태를 라우트에
묶을 수 있습니다 (예: 언어 바인딩의 클로저). 매칭, `:param` 경로, 미들웨어, 모든 전송 계층은
`cwist_app_get()`과 똑같이 동작합니다.

```c
typedef void (*cwist_handler_ex_func)(void *user_ctx, cwist_http_request *req,
                                      cwist_http_response *res);
typedef void (*cwist_handler_ctx_destroy_func)(void *user_ctx);

cwist_error_t cwist_app_get_ex(cwist_app *app, const char *path, cwist_handler_ex_func handler,
                               void *user_ctx, cwist_handler_ctx_destroy_func destroy);
```

호출할 때마다 `user_ctx`의 소유권은 앱으로 넘어갑니다. `destroy`가 NULL이 아니면
`user_ctx`를 인자로 정확히 한 번 호출됩니다.

- 앱이 파괴될 때;
- 같은 메서드와 경로(`:param` 세그먼트가 없는 경로)가 어떤 라우팅 함수로든 다시 등록될 때.
  단, 새 등록이 같은 NULL이 아닌 `user_ctx`를 넘기면 호출되지 않습니다;
- 등록이 실패하면 즉시. 이때 함수는 INT16 `-1`을 반환하고, 성공은 INT16 `0`입니다.

멀티포트 서브 앱은 컨텍스트를 소유하지 않고 공유합니다. 소유권을 호출자에게 남기려면
`destroy = NULL`을 넘기세요. 핸들러는 여러 워커 스레드에서 동시에 실행될 수 있으므로, 공유
컨텍스트는 자체 동기화가 필요합니다.

```c
typedef struct { const char *greeting; } greeter;

static void greet(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    const greeter *g = user_ctx;
    (void)req;
    cwist_sstring_assign(res->body, g->greeting);
}

greeter *g = cwist_alloc(sizeof(*g));
g->greeting = "hello";
cwist_app_get_ex(app, "/greet", greet, g, cwist_free);
```

### `cwist_app_dispatch_memory`
메모리 버퍼에 담긴 원시 HTTP/1.x 요청에 대해 라우터/미들웨어/핸들러 파이프라인 전체를
실행합니다. 소켓, 스레드, 이벤트 루프가 필요 없습니다. 응답은 새로 할당한 버퍼에 직렬화되며
(상태 줄 + 헤더 + 본문), 호출자가 `cwist_free()`로 해제합니다. 단발성 `Connection: close`
의미를 가지며, WASM 호스트 같은 내장 전송 계층을 위한 인메모리 진입점입니다.

```c
char *res_buf;
size_t res_len;
const char *req = "GET /users/7 HTTP/1.1\r\nHost: x\r\n\r\n";
if (cwist_app_dispatch_memory(app, req, strlen(req), &res_buf, &res_len) == 0) {
    fwrite(res_buf, 1, res_len, stdout);
    cwist_free(res_buf);
}
```


### `cwist_app_ws`
WebSocket 핸들러를 등록합니다.
```c
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);
```

파라미터가 없는 라우트는 해시 테이블에 저장되어 O(1)로 조회되고, 파라미터가 있는 패턴은 순차 매칭으로 처리됩니다.

### `cwist_endpoint_opt_t`
각 라우트에는 동작 플래그 비트마스크를 붙일 수 있습니다.

| 플래그 | 설명 |
| ---- | ----------- |
| `CWIST_DYNAMIC` | 기본 동적 핸들러 동작. |
| `CWIST_ENDPOINT_FIXED` | 라우트의 응답이 요청과 무관하다는 힌트. **현재 리비전에서는 캐싱을 켜지 않습니다.** 모든 요청은 평소처럼 디스패치됩니다. `docs/fixed-cache-status.md`와 ADR-0001(초안 PR #85)을 보세요. |
| `CWIST_ENDPOINT_FILE` | 엔드포인트가 파일을 스트리밍한다는 힌트로, Linux/BSD의 `sendfile` 빠른 경로를 켭니다. |

플래그는 `_opt` 도우미로 설정합니다.

```c
cwist_app_get_opt(app, "/feed", feed_handler, CWIST_ENDPOINT_FIXED);
cwist_app_get_opt(app,
                  "/download/:id",
                  download_handler,
                  CWIST_DYNAMIC | CWIST_ENDPOINT_FILE);
```

플래그는 OR로 묶어 동작을 조합할 수 있습니다 (예: `fixed + file`).

## 정적 자산

### `cwist_app_static`
```c
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *dir);
```
디렉터리를 URL 접두사에 마운트합니다 (경로 정규화 + 경로 순회 방지). 정적 응답도 미들웨어 체인을 거치며, MIME 감지, 경로 순회 방지, HEAD를 고려한 `Content-Length`를 위해 `cwist_http_response_send_file`을 사용합니다.

## 에러 처리

### `cwist_app_set_error_handler`
상태 코드 400 이상(예: 404 Not Found)에 대한 전역 에러 핸들러를 등록합니다.
```c
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);
```

### 사용하지 않는 변수
핸들러에서 쓰지 않는 파라미터에 대한 컴파일러 경고는 `CWIST_UNUSED()` 매크로로 끕니다.
```c
void my_handler(cwist_http_request *req, cwist_http_response *res) {
    CWIST_UNUSED(req);
    cwist_sstring_assign(res->body, "Hello");
}
```

## 동시성과 스레드 안전성

- **멀티스레딩**: `cwist_app_listen`은 멀티스레드 서버를 시작합니다 (요청당 스레드 하나). 핸들러와 미들웨어는 반드시 스레드 안전해야 합니다.
- **전역 상태**: 핸들러에서 전역 변수를 피하세요. 공유 상태가 필요하면 적절한 동기화 수단(예: `pthread_mutex_t`)으로 보호해야 합니다.
- **컨텍스트 전달**: 각 핸들러는 공유 인프라로 `req->app`/`req->db`를 받으며, `req->private_data`는 미들웨어 간 통신용으로 남아 있습니다.

## 메모리 소유권 규칙

- **프레임워크 소유**: 핸들러에 전달되는 `cwist_http_request`와 `cwist_http_response` 객체는 프레임워크가 소유합니다. 핸들러 안에서 파괴하지 마세요.
- **빌린 문자열**: `cwist_http_header_get`이나 `cwist_json_get_raw`가 반환하는 문자열은 빌린 것입니다. 빌더나 요청이 파괴된 뒤에도 필요하면 복사해야 합니다.
- **명시적 파괴**: `_create`로 만든 객체(예: `cwist_json_builder_create`, `cwist_websocket_upgrade`)는 호출자가 대응하는 `_destroy` 함수로 파괴해야 합니다.

### `cwist_app_listen`
```c
int cwist_app_listen(cwist_app *app, int port);
```
지정한 포트에서 서버 루프를 시작합니다.

## 인메모리 테스트 클라이언트

*헤더:* `<cwist/sys/app/test_client.h>`

`cwist_test_client`는 소켓을 열지 않고 합성 HTTP 요청을 앱의 라우터와 미들웨어로 직접
보냅니다. 그래서 통합 테스트가 포트도, 종료 경합도 없이 프로세스 안에서 실행됩니다.

### `cwist_test_client_create` / `cwist_test_client_destroy`
```c
cwist_test_client *cwist_test_client_create(cwist_app *app);
void               cwist_test_client_destroy(cwist_test_client *client);
```
`app`에 묶인 테스트 클라이언트를 만들고 파괴합니다. 클라이언트는 인스턴스별 쿠키 저장소를
유지하며, 이후 요청에 자동으로 실어 보냅니다.

### 요청 도우미
```c
cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path);
cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path,
                                            const char *body, const char *content_type);
cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path,
                                                 const char *json_body);
cwist_http_response *cwist_test_client_put(cwist_test_client *client, const char *path,
                                           const char *body, const char *content_type);
cwist_http_response *cwist_test_client_delete(cwist_test_client *client, const char *path);
cwist_http_response *cwist_test_client_patch(cwist_test_client *client, const char *path,
                                             const char *body, const char *content_type);
```
각 함수는 호출자가 소유하는 `cwist_http_response *`를 반환합니다.
`cwist_http_response_destroy()`로 해제하세요.

헤더, 쿠키, 쿼리 문자열을 모두 제어하려면 다음을 사용합니다.
```c
cwist_http_response *cwist_test_client_request_ex(cwist_test_client *client,
                                                  cwist_http_method_t method, const char *path,
                                                  const cwist_test_client_request_options *opts);
```

### 쿠키 저장소
```c
void        cwist_test_client_set_cookie(cwist_test_client *client,
                                         const char *name, const char *value, const char *path);
const char *cwist_test_client_get_cookie(cwist_test_client *client, const char *name);
void        cwist_test_client_clear_cookies(cwist_test_client *client);
```

### 테스트 단언 매크로
```c
CWIST_ASSERT_STATUS(res, expected_status)
CWIST_ASSERT_HEADER(res, header_name, expected_value)
CWIST_ASSERT_BODY_CONTAINS(res, snippet)
```
각 매크로는 실패하면 파일과 줄 번호가 포함된 `[ASSERT FAIL]` 메시지를 출력하고
`exit(1)`을 호출합니다. 프레임워크 자체 테스트 스위트와 같은 방식입니다.
