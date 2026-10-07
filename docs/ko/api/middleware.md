# 미들웨어 API

미들웨어를 사용하면 요청이 최종 핸들러에 도달하기 전에 가로채서 처리할 수 있습니다.

## 함수

### `cwist_app_use`
전역 미들웨어를 등록합니다. 미들웨어는 등록한 순서대로 실행됩니다.
```c
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);
```

## 내장 미들웨어

### Request ID 미들웨어
요청마다 고유 ID를 만들고 요청과 응답 양쪽의 `X-Request-Id` 헤더에 넣습니다.
```c
#include <cwist/sys/app/middleware.h>
cwist_app_use(app, cwist_mw_request_id(NULL));
```

### 액세스 로그 미들웨어
요청 정보(메서드, 경로, 상태, 지연 시간)를 stdout에 기록합니다.
`cwist_async_defer()`로 연기된 응답은 체인이 반환될 때 상태와 크기를 알 수 없으므로 `-`로(JSON 형식에서는 `null`로) 기록됩니다.
```c
cwist_app_use(app, cwist_mw_access_log(CWIST_LOG_COMBINED));
```

### 속도 제한 미들웨어
단일 IP의 분당 요청 수를 제한합니다.
```c
cwist_app_use(app, cwist_mw_rate_limit_ip(60));
```

### CORS 미들웨어
교차 출처 리소스 공유(CORS)를 지원합니다.
- 모든 응답에 `Access-Control-Allow-Origin: *`를 추가합니다.
- `OPTIONS` 사전 요청(preflight)은 204 No Content와 적절한 헤더로 처리하고 요청 처리를 그 자리에서 끝냅니다.
```c
cwist_app_use(app, cwist_mw_cors());
```

### Prometheus 메트릭 미들웨어
Prometheus 노출 형식의 `/metrics` 엔드포인트를 제공합니다. 요청 수, 지연 시간 히스토그램, 활성 연결을 추적합니다.
```c
#include <cwist/sys/app/middleware.h>
cwist_app_use(app, cwist_mw_metrics());
```

### JWT 인증 미들웨어
`Authorization` 헤더의 `Bearer` 토큰을 HMAC-SHA256으로 검증합니다. 실패하면 `401 Unauthorized`로 응답하고 체인을 중단합니다. 디코딩된 클레임은 `cwist_mw_jwt_get_claims()`로 이후 핸들러에서 사용할 수 있습니다.
```c
cwist_app_use(app, cwist_mw_jwt_auth("my-secret"));

// 이 미들웨어 뒤에 있는 핸들러 안에서:
const cwist_jwt_claims *claims = cwist_mw_jwt_get_claims(req);
```
`secret`은 NULL로 끝나는 문자열이어야 하며, 미들웨어가 호출되는 동안 계속 살아 있어야 합니다 (보통 static 또는 전역 문자열).

### 압축 미들웨어
`Accept-Encoding`을 확인하고, 등록된 백엔드(gzip/zstd)로 응답 본문을 압축한 뒤 `Content-Encoding`을 추가합니다. 본문이 `min_body_size` 바이트 이상일 때만 압축합니다. `cwist_async_defer()`로 연기된 응답은 압축하지 않고 보냅니다. 핸들러가 연기한 순간부터 그 응답은 완료 처리 쪽에 속하기 때문입니다.
```c
cwist_app_use(app, cwist_mw_compress(1024)); /* 1 KiB 이상 응답을 압축 */
```

## 사용자 정의 미들웨어 만들기
미들웨어는 `req`, `res`, `next` 콜백을 받는 함수입니다.

**스레드 안전성 참고**: 미들웨어는 멀티스레드 환경에서 실행됩니다. 공유 자원에 대한 접근은 반드시 동기화해야 합니다.

```c
static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
static int total_requests = 0;

void my_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    pthread_mutex_lock(&count_mutex);
    total_requests++;
    pthread_mutex_unlock(&count_mutex);
    
    next(req, res);
}
```
