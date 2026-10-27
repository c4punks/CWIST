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

#### 비동기 로그 출력

기본적으로 액세스 로그 한 줄과 `cwist_logger_*` 한 줄은 요청을 처리하는 스레드가 stdout이나 stderr에 직접 쓰며, 이때 stdio 블록 버퍼링을 사용합니다(TTY면 줄 버퍼링을 유지). 따라서 한 줄씩 즉시 전달되지 않고 묶음으로 전달될 수 있습니다. 시작할 때 `cwist_log_async_start()`를 한 번 호출하면 백그라운드 스레드 하나가 대신 쓰며, 이것이 한 줄 즉시 전달을 얻는 방법이기도 합니다. 그러면 요청 스레드는 줄을 만들어 버퍼에 복사하기만 합니다.

```c
#include <cwist/sys/app/logger.h>

cwist_log_async_start(0);   /* 0 = 스트림마다 256 KiB 버퍼 */
cwist_app_use(app, cwist_mw_access_log(CWIST_LOG_COMBINED));
cwist_app_listen(app, 8080);
cwist_log_async_stop();     /* 아직 쌓여 있는 줄을 씁니다 */
```

- **줄 단위:** 각 줄은 잘리지 않고, 같은 스트림의 줄은 기록한 순서를 유지합니다.
- **버퍼가 찼을 때:** 호출한 쪽이 빈 공간이 생길 때까지 기다립니다. 줄을 버리지 않습니다.
- **긴 줄:** 버퍼보다 큰 줄은 동기로 쓰기 때문에 순서가 어긋날 수 있습니다.
- **종료:** `exit()` 시점에 남은 줄은 `atexit` 훅이 씁니다. `_exit()` 전에는 `cwist_log_async_flush()`를 호출하세요.
- **워커:** fork된 워커(`CWIST_WORKERS`)는 각자 첫 줄을 쓸 때 writer를 따로 시작합니다.
- **섞어 쓰기:** 같은 스트림에 `printf`로 쓴 출력과 버퍼에 쌓인 줄 사이의 순서는 보장하지 않습니다.
- **WASM:** 지원하지 않습니다. `cwist_log_async_start()`가 `false`를 반환하고 동기 출력을 유지합니다.

### 속도 제한 미들웨어
단일 IP의 분당 요청 수를 제한합니다.
```c
cwist_app_use(app, cwist_mw_rate_limit_ip(60));
```

### Redis 기반 공유 rate limiter

기존 limiter는 프로세스별로 버킷을 보관합니다. 여러 독립 프로세스가
한도를 공유하려면 각 프로세스에서 **DB 0, default 사용자, AUTH 없는
전용 풀**을 따로 생성하고 같은 prefix와 한도를 사용합니다.
재연결은 AUTH·SELECT·HELLO 등 연결 상태를 복원하지 않습니다. timed API가
소켓을 폐기한 뒤에는 같은 풀의 기존 API도 새 연결을 받을 수 있습니다.
정책마다 다른 prefix를 쓰세요. 같은 prefix에 서로 다른 한도를 주면
독립 버킷이 아니라 같은 카운터를 공유합니다.

```c
cwist_redis_pool_t *pool = cwist_redis_pool_create("127.0.0.1", 6379, 4);
cwist_rate_limit_redis_cfg_t cfg = {
    .pool = pool,
    .requests_per_minute = 60,
    .key_prefix = "my-service:rate:",
    .timeout_ms = 50,
    .fail_closed = true,
};
cwist_middleware_func limit = cwist_mw_rate_limit_ip_redis(&cfg);
/* main 함수 안: 등록 실패 시 서버를 시작하지 않습니다. */
if (!pool || !limit) return 1;
cwist_app_use(app, limit);
/* 인스턴스마다 한 프로세스: 생성된 풀을 다시 fork하지 않습니다. */
cwist_app_listen_ex(app, 8080, 1, -1);
cwist_redis_pool_destroy(pool);
```

키는 `prefix + 실제 소켓 peer IP`입니다. Lua EVAL 한 번으로 카운트를
증가시키고 첫 요청부터 60초 TTL을 설정합니다. TTL이 없으면 복구하며,
진행 중인 윈도는 연장하지 않습니다. 허용·차단 요청 모두 카운트합니다.
한도 초과는 429와 보수적인 `Retry-After: 60`을 반환합니다.

`fail_closed = false`가 기본값이며 Redis 오류가 나면 요청을 허용합니다.
`true`이면 429가 아닌 503을 반환합니다. 타임아웃, 풀 고갈, 잘못된 응답,
peer 주소 누락도 이 정책을 따릅니다. 로컬 카운터로 대체하지 않습니다.
실패한 명령이 이미 카운트를 올렸을 수 있으므로 자동 재시도하지 않습니다.

요청 지연 예산은 풀 대기, 캐시된 주소로의 재연결, 송신·응답 I/O를
포함합니다. 0은 기본 50ms이며 최대 60000ms입니다. 비동기 reactor API가
아니라 제한 시간 내 동기 호출입니다. 스케줄링 지연까지 실시간으로
보장하지 않습니다. 초기 풀 생성과 DNS는 예산 밖이며, 생성 실패나
잘못된 설정은 factory가 NULL을 반환합니다. 설정별·프로세스별 첫 backend
오류 때 오류 코드와 장애 정책을 stderr에 한 번 기록합니다. 키나 자격
증명은 기록하지 않습니다. Redis I/O 예산은 애플리케이션 로그의 지연을
제한하지 않습니다.

풀은 fork 이후 각 서비스 프로세스에서 따로 생성해야 합니다. 상속한
풀은 timed API가 거부합니다. 모든 호출이 끝나기 전에는 풀을 파괴하면
안 됩니다. 자동 `CWIST_WORKERS` fork에 pre-fork 풀을 그대로 넘기면 안
됩니다. 이 API는 worker-start hook을 추가하지 않습니다. 서로 다른
설정은 프로세스당 8개까지이며 같은 설정은 슬롯을 재사용합니다.
복사되는 prefix는 최대 128바이트입니다.

Forwarded 헤더는 키에 쓰지 않습니다. 연결된 TCP peer fd가 없는 요청
(예: QUIC 요청)은 peer 누락 장애 정책을 따릅니다. QUIC path metadata를
추출하는 API는 아닙니다. IPv6는 /64 네트워크가 아닌 전체 /128 주소별
키를 사용하므로 주소를 교체하면 별도 버킷을 얻게 됩니다.
reverse proxy 뒤에서는 신뢰된
upstream이 원래 source 주소를 보존하지 않는 한 proxy IP를 제한합니다.
RESP2는 평문이므로 loopback·사설망·별도 TLS 터널에서 사용하세요.
default 사용자 ACL에 EVAL, INCR, PTTL, PEXPIRE가 필요합니다. AUTH가 필요한
서버에서는 모든 호출이 실패하고 매 요청에 장애 정책이 적용됩니다.
Redis eviction, failover, 외부 키 삭제가 카운터를 초기화할 수 있습니다.

`redis-server`를 설치하고 `make redis-rate-limit-e2e`를 실행하세요.
runner는 자체 loopback Redis를 띄우며 통합 검증을 묵시적으로 건너뛰지
않습니다. 두 C 프로세스가 별도 풀과 실제 TCP peer fd로 미들웨어를
호출해 40번 중 합계 7번만 허용되는지 검사합니다. TTL·만료·namespace·
장애 정책도 검사합니다. 일반 unit target은 테스트 포트가 없으면
live-Redis 부분만 건너뜁니다.

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
