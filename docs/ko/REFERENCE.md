# CWIST 라이브러리 레퍼런스

CWIST는 안전하고 확장 가능한 웹 애플리케이션을 만들기 위한 C 라이브러리입니다. 문자열 처리,
HTTP/HTTPS, URI 파싱, 데이터베이스 접근, 암호학적 해시를 위한 모듈을 포함합니다.

## 모듈

### 1. SString (스마트 문자열)
안전한 동적 문자열 구현입니다.
- **헤더:** `<cwist/core/sstring/sstring.h>`
- **기능:** 자동 크기 조절, 이어 붙이기, 안전한 조작.

### 2. HTTP 서버
멀티 프로세스/스레드 HTTP 서버 프레임워크입니다.
- **헤더:** `<cwist/net/http/http.h>`
- **구조체:** `cwist_http_request`, `cwist_http_response`
- **기능:**
  - 요청 파싱
  - 응답 직렬화
  - 헤더/상태 코드 도우미 함수.

### 3. HTTPS 지원
포스트 양자 암호를 지원하는 BoringSSL 기반 보안 전송 계층입니다.
- **헤더:** `<cwist/net/http/https.h>`
- **기능:**
  - `cwist_https_init_context`: TLS 1.3 이상을 강제하며 인증서/키를 불러옵니다.
  - `cwist_https_accept`: SSL 핸드셰이크.
  - `cwist_https_send_response`: 암호화된 전송.
  - `cwist_app_use_pqc_layer`: 하이브리드 PQC TLS(`X25519MLKEM768:X25519:P-256`)를 한 줄로 켜고, TLS 1.3 미만을 비활성화합니다.
  - **참고:** 내부적으로 로직은 `cwist/http.h`를 재사용하고 보안 계층을 덧붙입니다.

### 4. 쿼리 파싱
`liburiparser`를 사용하는 견고한 쿼리 문자열 파싱입니다.
- **헤더:** `<cwist/net/http/query.h>`
- **함수:** `void cwist_query_map_parse(map, raw_query)`
- **동작:** `key=value&key2=val2`를 해시 맵(SipHash)으로 파싱합니다.

### 5. 데이터베이스 (SQL)
영구 저장을 위한 `sqlite3` 래퍼입니다.
- **헤더:** `<cwist/core/db/sql.h>`
- **기능:**
  - `cwist_db_open`: DB 파일에 연결합니다.
  - `cwist_db_exec`: 명령을 실행합니다 (CREATE/INSERT/UPDATE).
  - `cwist_db_query`: SELECT를 실행하고 결과를 `cJSON` 배열로 받습니다.

### 6. SipHash
해시 맵에 쓰이는 암호학적 해시 함수입니다 (쿼리/헤더에서 사용).
- **헤더:** `<cwist/core/siphash/siphash.h>`

### 7. 에러 처리
`cwist_error_t`를 사용하는 통합 에러 처리 시스템입니다.
- **헤더:** `<cwist/sys/err/cwist_err.h>`
- **기능:** 단순한 정수 코드나 복잡한 JSON 객체(예: OpenSSL/SQLite 에러)를 반환할 수 있습니다.

### 8. Big Dumb Reply (BDR)
비용이 큰 핸들러의 직렬화된 응답을 자동으로 캐시합니다.
- **헤더:** `<cwist/sys/app/big_dumb_reply.h>`
- **함수:** `cwist_bdr_get`, `cwist_bdr_put`, `cwist_bdr_set_limits`.
- **안전장치:** 항목은 설정 가능한 TTL이나 히트 횟수 예산이 지나면 만료되고, 캐시는 소프트 바이트 상한(기본 32 MiB)을 유지합니다. 애플리케이션별 동작은 `cwist_app_configure_bdr`로 조정합니다.

## LibTTAK 메모리 기능

CWIST는 libttak을 내장하고 최신 메모리 런타임을 노출합니다.

- **세대별 아레나(Generational Arenas):** 정적 자산과 캐시 항목을 `ttak_mem_tree`로 추적하고 세대 단위로 해제해 RSS 급증을 피합니다.
- **에포크 기반 회수(Epoch-Based Reclamation):** 핫 리로드와 Big Dumb Reply 교체 시 `ttak_epoch_enter/exit`를 호출하므로, 아레나가 교체되는 동안에도 읽는 쪽은 막히지 않습니다.
- **분리 가능한 메모리(Detachable Memory):** `ttak_detachable_mem_alloc`이 제로 카피 HTTP 본문을 뒷받침합니다. 캐시된 조각은 작은 detachable 캐시를 재사용하고 `cwist_http_response_set_body_ptr_managed`로 정리 훅을 등록합니다.

세 가지를 모두 조합해 부하 테스트 중에도 `/rps`를 포화 상태로 유지하는 실행 가능한 예제는 `example/rps-showcase/`에 있습니다.

## 예제: Othello Web
위치: `example/othello-web/`.
- **스택:** HTML/CSS/JS (프론트엔드), C (백엔드).
- **기능:**
  - HTTPS (포트 8443).
  - 멀티플레이어 (SQLite 기반).
  - 방 지원 (`?room=ID`).
  - 전략 힌트 (프론트엔드 JS).

## 의존성
- `libssl-dev` (OpenSSL)
- `libcjson-dev` (JSON)
- `liburiparser-dev` (URI 파싱)
- `libsqlite3-dev` (데이터베이스)

## 빌드
`make`를 실행하면 정적 라이브러리 `libcwist.a`가 만들어집니다.
`make test`를 실행하면 단위 테스트가 돌아갑니다.
