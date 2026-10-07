# WASM 위의 CWIST (Emscripten)

CWIST는 Emscripten으로 WebAssembly에 컴파일해 JS에서 완전히 구동할 수 있습니다. HTTP 요청이
바이트로 들어가고, 라우터 + 미들웨어 + 핸들러가 WASM 안에서 실행되며, 직렬화된 HTTP 응답이 WASM
힙 위의 제로 카피 `Uint8Array` 뷰로 돌아옵니다.

이 페이지는 범위에 들어가는 것, 빌드 방법, 그리고 두 연동 지점(`cwist_app_dispatch_memory`와
TypedArray 도우미)을 설명합니다. 완전하고 실행 가능한 저수준 예제는 `tests/wasm_smoke.c`를,
Service Worker 호스트 뒤의 전체 앱은 `example/wasm-service-worker/`를 보세요.

## 빌드하기

Emscripten 도구 체인(`emcc`/`emar`)과 스모크 테스트를 위한 `node`가 필요합니다.

```sh
make wasm               # produces libcwist_wasm.a
make wasm-smoke         # builds and runs tests/wasm_smoke.c under node
```

WASM 아카이브는 네이티브 라이브러리와 별개인 더 작은 빌드입니다. `make test`와 `make all`에는
**포함되지 않습니다**.

## WASM 빌드에 들어 있는 것

소켓과 무관한 서브시스템만 들어 있습니다.

- 앱 디스패치: 라우터(`mux`), 미들웨어 파이프라인, 설정, 로거, 인메모리 요청 디스패치(`app.c`)
- HTTP/1.1 파서와 직렬화기, 쿼리 문자열, 쿠키, 세션
- `sstring`, 아레나와 할당자, siphash
- JSON (`json_builder`, `json_heal`, cJSON)
- 검증 (`zod`, `validation/bind`)
- 템플릿과 HTML(`template`, `html/builder`, `html/component`, `html/css_composer`),
  페이지/프래그먼트 응답(`net/http/html_response`), 콘텐츠 해시 기반 인메모리 자산
  (`sys/app/assets`). [html.md](html.md#wasm에서-같은-뷰-렌더링하기) 참고
- blob 형태 데이터베이스를 위한 `cwist_db_open_memory()`와 `cwist_db_serialize()`를 포함한
  `cwist_db` (SQLite)

의도적으로 제외한 것: 소켓과 accept 루프, TLS/BoringSSL, QUIC/HTTP/3, gRPC(HTTP/2 필요),
WebSocket 전송, 스레드/스케줄러, 압축, DB 풀/동기화 클라이언트.

## JS에서 요청 디스패치하기

`cwist_app_dispatch_memory()`는 요청 하나 전체를 앱에서 실행하고 직렬화된 HTTP 응답을 돌려줍니다.
요청 전체를 넣고 응답 전체를 받는 방식입니다. 스트리밍 전달은 아래의 `cwist_app_dispatch_stream()`과
`cwist_stream_req_begin/feed/end`를 보세요.

```c
char *res_buf = NULL;
size_t res_len = 0;
cwist_app_dispatch_memory(app, req_buf, req_len, &res_buf, &res_len);
/* res_buf is heap-allocated; free it with cwist_free() when done */
```

## 제로 카피 뷰: `cwist/wasm/typedarray.h`

이 헤더(Emscripten 빌드 전용)를 포함하면 C 배열을 JS에 타입 배열 뷰로 노출하고 `Module.cwistView`
도우미 묶음을 설치할 수 있습니다.

```c
static const int32_t g_samples[] = {10, -20, 30, 40};
CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)
CWIST_WASM_INSTALL_VIEWS()
```

`CWIST_WASM_INSTALL_VIEWS()`는 번역 단위 하나에서 한 번만 전개하고, JS 콜백이 실행되기 전에
`main()`에서 한 번 호출해야 합니다. 다음을 설치합니다.

- `Module.cwistView.u8(ptr, n)`  -> `Uint8Array`
- `Module.cwistView.i32(ptr, n)` -> `Int32Array`
- `Module.cwistView.f64(ptr, n)` -> `Float64Array`

모든 뷰는 WASM 힙을 직접 가리킵니다 (복사 없음). `CWIST_WASM_EXPOSE_*`는 JS에서 호출할 수 있는
`_<name>_ptr()` / `_<name>_len()` 접근자를 생성합니다.

## WASM에서의 데이터베이스

SQLite는 `libcwist_wasm.a`에 컴파일되어 들어가므로, `cwist_db`는 WASM 인스턴스 밖에 상태를
저장하는 엣지 호스트에 알맞은 blob 지향 진입점 두 개와 함께 WASM에서 동작합니다.

- `cwist_db_serialize(db, &out, &out_len)`는 데이터베이스 이미지를 호출자가 소유하는 버퍼로
  내보냅니다 (`cwist_free()`로 해제).
- `cwist_db_open_memory(&db, buf, len, readonly)`는 직렬화된 이미지로 데이터베이스를 엽니다.
  이미지는 복사되므로 호출자는 버퍼를 계속 가집니다. NULL이 아닌 이미지를 넘기세요. "아무것도
  없는 것"을 여는 것은 설계상 에러입니다.

일반적인 왕복: `:memory:`를 열고, 마이그레이션을 적용하고, 읽기를 제공한 다음, 호스트로 다시
직렬화하거나 호스트 이미지의 읽기 전용 복사본을 엽니다. `cwist_db_query()`는 모든 값을 문자열로
담은 객체들의 cJSON 배열로 행을 반환합니다 (SQLite `exec` 콜백 의미).

## `cwist_db`가 번들 크기에 주는 영향

`src/core/db/db.c` + `lib/sqlite3/sqlite3.c`를 추가하면 SQLite 전체가 아카이브에 들어옵니다.
Emscripten 5.0.0으로 이 브랜치와 db 이전 트리를 비교해 측정했습니다.

| 산출물 | 이전 | 이후 | 비율 |
|---|---|---|---|
| `libcwist_wasm.a` | 328,518 B | 1,703,706 B | 5.2x |
| 링크된 `wasm_smoke.wasm` | 67,267 B | 1,052,405 B | 15.7x |

아카이브는 5배 커지지만 링크된 스모크 바이너리는 16배 커집니다. 스모크 테스트 자체가 db 왕복을
하기 때문입니다 (cJSON 조회 결과 생성도 함께 링크에 들어옴). 소비자에게 미치는 측정된
결과(2026-09-21): 아카이브 링크는 객체 단위이고 핵심 WASM 객체 중 `cwist_db_*`를 참조하는 것이
없으므로, db를 쓰지 않는 앱은 약 64.8 KB로 링크됩니다 (db 이전 크기). `cwist_db_*`를 호출하는
앱만 약 1 MB의 amalgamation을 끌어오며, 그것이 SQLite의 도달 가능한 핵심입니다.
`-ffunction-sections` + `--gc-sections`로 되찾는 것은 약 300 B뿐인데, emcc -O2가 이미 모듈 간
DCE를 하기 때문입니다.

**결정 (이슈 #93): 옵트아웃/분리 빌드 없음.** 링크 크기 비용은 db 사용자에게만 떨어지며 그것은
SQLite 고유의 비용입니다. 분리된 아카이브는 그 비용을 줄이지 못하면서 db 소비자가 아카이브 두
개를 링크하게 만듭니다. 약 1 MB가 문제가 된다면, 손잡이는 패키징이 아니라 `SQLITE_OMIT_*` 기능
생략입니다.

## 세션과 쿠키

세션은 **클라이언트 쪽 서명 쿠키**(base64 JSON 페이로드에 대한 HMAC-SHA256)이므로 세션 상태가
요청과 함께 이동하고 인스턴스 수명은 상관없습니다. WASM 인스턴스를 넘어 살아남아야 하는 것은 서명
비밀 값뿐입니다. Phase 3 영속성 모델(이슈 #93):

- **비밀 값을 고정하세요.** 호스트가 보존하는 비밀 값(예: fetch 계층에 박아 넣은 상수, 또는
  시작 시 가져오는 KV/localStorage 값)으로 `cwist_app_use_session(app, secret)`을 호출하세요.
  부팅마다 생성되는 비밀 값은 호스트가 모듈을 재활용할 때마다 모든 세션을 무효화합니다. 자동
  생성은 프로세스가 계속 사는 네이티브 서버를 위한 편의 기능이며, WASM에서는 `/dev/urandom`이
  없을 때 `crypto.getRandomValues` / `getentropy()`로 대체됩니다. 데모에는 괜찮지만 프로덕션에는
  맞지 않습니다.
- **JS 주입 지점.** `CWIST_WASM_DEFINE_ENTRY`로 빌드한 모듈은 `_cwist_wasm_use_session(secret)`을
  내보냅니다. npm 래퍼는 이를 `handle.useSession(secret)`으로 노출하며 (세션을 쓰는 첫 디스패치
  전에 호출; 무작위 개발용 비밀 값을 쓰려면 `null`), 바인딩 시점에 선언적인
  `Module.cwistSessionSecret` 문자열도 한 번 적용합니다. 비밀 값을 교체하면 기존 세션이 모두
  무효화되며, 강제 로그아웃은 이렇게 구현합니다.
- **암호는 함께 들어 있습니다.** WASM 빌드는 `include/cwist/core/crypto/sha256.h`의 헤더 전용
  SHA-256/HMAC으로 쿠키 서명을 검증하므로 (OpenSSL은 `libcwist_wasm.a`에 링크되지 않음), 이제
  Emscripten에서도 세션이 실제로 동작합니다. Phase 3 이전에는 아무것도 세션을 링크할 수 없었습니다.
- **쿠키는 호스트가 나릅니다.** Service Worker나 fetch 가로채기 호스트는 디스패치 응답의
  `Set-Cookie` 헤더를 자신의 쿠키 저장소에 복사하고, 이후 요청에 저장된 `Cookie` 헤더를 보내야
  합니다. 브라우저의 문서 쿠키 저장소는 SW가 처리하는 fetch 이벤트에 자동으로 공급되지 않습니다.

`tests/wasm_stream.c`(네이티브), Emscripten 스모크 테스트, `make wasm-wrapper-test`(JS 쪽
`useSession` 왕복 + 교체된 비밀 값 거부)로 종단 간 검증했습니다. 한 앱 인스턴스에서 설정한 세션은
같은 고정 비밀 값을 쓰는 두 번째 인스턴스에서 다시 읽히고, 다른 비밀 값으로는 거부됩니다.

## 경계를 통과하는 스트리밍

`cwist_app_dispatch_memory()`는 요청 전체를 넣고 응답 전체를 받는 방식입니다. Phase 3는
**경계**에서의 스트리밍을 추가합니다 (핸들러는 여전히 응답 본문을 메모리에 만듭니다. 청크 생산자
핸들러 API는 이제 `cwist_http_response_stream_begin/write/end`로 존재하며, 아래 "스트리밍 생산자"를
보세요).

- `cwist_app_dispatch_stream(app, req, req_len, write_fn, ctx)`는 직렬화된 응답을 싱크 콜백으로
  전달합니다. 머리(상태 줄 + 헤더)가 먼저 오고, 그다음 본문이 최대 `CWIST_STREAM_CHUNK`(64 KiB)
  조각으로 옵니다. 싱크가 0이 아닌 값을 반환하면 디스패치를 중단합니다 (-2).
- `cwist_stream_req_begin/feed/end`는 요청 본문을 점진적으로 조립합니다 (연속된 호스트 버퍼 하나
  없이 큰 업로드). `Content-Length`가 필요하고 강제되며, 그다음 `cwist_stream_req_dispatch`가
  스트리밍 응답으로 디스패치합니다.

표준 진입 매크로의 경우, `CWIST_WASM_DEFINE_ENTRY`는 `_cwist_wasm_dispatch_stream`도 내보냅니다.
호스트가 `Module.cwistStreamChunk(ptr, len)`을 정의하면 각 청크를 그것으로 흘려보내므로, 청크를
`ReadableStream`으로 조립하거나 JS에 모을 수 있습니다.

## 실행 가능한 예제: Service Worker 앱

`example/wasm-service-worker/`(이슈 #93 Phase 4)는 라우팅, zod 검증, 템플릿 렌더링, `cwist_db`,
세션을 함께 쓰는, Service Worker fetch 가로채기 호스트 뒤의 종단 간 앱입니다.

- `app.c` - `build.sh`가 `app.js`/`app.wasm`으로 컴파일하는 CWIST 앱:
  `GET /`는 세션 방문 횟수와 항목 목록이 있는 템플릿 페이지를 렌더링하고, `POST /items`는 JSON
  본문을 zod로 검증한 뒤 인메모리 `cwist_db`에 삽입하고, `GET /items`는 행을 JSON으로 반환하고,
  `GET /items/image`는 직렬화된 SQLite 이미지(`cwist_db_serialize`, 엣지 호스트가 보존할 blob)를
  반환하며, 알 수 없는 라우트는 라우터의 404로 넘어갑니다.
- `sw.js` - 호스트: 같은 출처의 GET/POST fetch를 가로채고, 모듈의 진입점으로 디스패치하며
  (직렬화는 `wasm/npm/index.js`를 따르되 독립적이도록 인라인), `_cwist_wasm_use_session`으로 세션
  비밀 값을 고정하고, 세션 쿠키를 직접 나릅니다. Service Worker는 문서 쿠키 저장소를 보지
  못하므로, 디스패치 응답의 `Set-Cookie`를 인메모리 저장소에 잡아 두었다가 이후 요청에 `Cookie`
  헤더로 재생합니다.
- `smoke.js` - cwist-wasm 래퍼를 통해 같은 모듈을 대상으로 하는 node 스모크. 200/400/201/404
  경로, db 이미지 엔드포인트, 모듈 인스턴스를 넘어 세션이 살아남는지를 다룹니다. Service Worker
  자체는 브라우저에서 수동으로 검증합니다 (단계는 예제의 README에 있음).

CI(`.github/workflows/wasm.yml`)에서 빌드하고 실행합니다. 빌드와 로컬 서빙 방법은 예제의
README.md를 보세요.

## (아직) 다루지 않는 것

- WASI 0.2(`wasm32-wasip2`)는 이제 지원되고 CI 관문이 있습니다. `docs/api/wasi.md`를 보세요.
  Cloudflare Workers와 Fastly Compute는 아직 평가하지 않았으며, 여기의 모든 내용은 여전히
  Emscripten `Module` 호스트를 가정합니다.
- 공개된 npm 패키지 / 릴리스 산출물. 지금은 모든 소비자가 `make wasm`으로 소스에서 빌드합니다.
- WASM CI. `wasm-smoke`는 수동 확인이므로, `WASM_SRCS`, `typedarray.h`, `EM_JS` 아래의 무엇이든
  건드리기 전에 실행하세요.

## 스트리밍 생산자 (핸들러가 본문을 청크 단위로 생성)

`cwist_http_response_stream_begin/write/end()`(이슈 #201 Phase 1)는 버퍼링 생산자의 빈틈을
메웁니다. 핸들러가 응답을 청크 모드로 바꾸고, 본문을 점진적으로 쓰고, 스트림을 끝냅니다.

- `cwist_app_dispatch_memory()`에서는 프레이밍된 바이트가 버퍼에 쌓였다가 다른 본문처럼
  직렬화됩니다 (`Transfer-Encoding: chunked`, 청크 프레이밍은 쓰는 시점에 적용).
- `cwist_app_dispatch_stream()`에서는 각 쓰기가 즉시 호스트 싱크에 도달하며 머리가 먼저 갑니다
  (첫 쓰기 때 지연 전송). 그래서 SSE 방식 핸들러가 실행 중에도 청크를 전달합니다. 백프레셔는 싱크가
  쓰기를 거부하는 것입니다 (디스패치에서 -2).
- `begin` 전에 대입한 본문은 버려지고, `end` 이후의 쓰기는 실패하며, `end` 없이 반환한 핸들러는
  암묵적으로 마무리됩니다.
- `cwist_http_response_stream_write(res, data, 0)`은 아무것도 하지 않습니다 (빈 청크는 청크 종료
  표시이므로 스트림 중간에 내보내지 않음).
