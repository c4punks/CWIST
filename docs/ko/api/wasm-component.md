# WASM 컴포넌트 모델 위의 CWIST

**상태:** 실험적 (이슈 #203, v3.7 Phase 1; v3.8은 프로덕션 전환을 v4.0으로 미룸). 지원되는
브라우저 경로는 여전히 Emscripten 번들(`docs/api/wasm.md`)입니다. 이 문서는 WASI 0.3 world와
플래그 없는 JSPI를 끝까지 쓸 수 있게 되면 그것을 대체할 예정인 컴포넌트 파이프라인을 설명합니다.

## 이유

v3.6은 WASM 종류 세 가지를 냈습니다. Emscripten 브라우저 번들(지원), WASI preview1 스모크(이
작업으로 폐기; WASI 0.2가 용도를 대신함), WASI 0.2 소켓 서버(`docs/api/wasi.md`)입니다. 도구 체인
세 개는 얻는 것보다 비용이 크고, Emscripten 경로는 운영상의 마찰을 낳습니다. 무거운 emsdk, 버전에
민감한 접착 코드, EM_JS 서식 사고(#176)가 그 예입니다. 최종 상태는 파이프라인 하나입니다.
wasi-sdk로 컴파일하고, 어디서나 실행하며, 브라우저는 jco로 변환한 JS 번들로, 엣지 런타임은
wasmtime으로 실행합니다.

## 경계

`wit/cwist.wit`는 `include/cwist/wasm/wasm_entry.h`의 컴포넌트 모델 대응물인
`world cwist-guest`를 정의합니다.

- `dispatch(request: list<u8>) -> result<list<u8>, dispatch-error>`: 직렬화된 HTTP/1.1이 들어가고
  나오며, 오늘날 `_cwist_wasm_dispatch`가 처리하는 것과 정확히 같습니다. 앱 쪽 파서는 바뀔 것이
  없습니다.
- `use-session(secret: option<string>)`: 호스트가 주입하는 세션 서명 비밀 값.
  `_cwist_wasm_use_session`과 같은 계약입니다.
- 지금은 명시적인 `_cwist_wasm_dispose` 왕복이 필요한 버퍼 해제가 암묵적이 됩니다. canonical
  ABI가 결과를 게스트 선형 메모리 밖으로 복사하고, 게스트는 반환 후 자기 버퍼를 해제합니다.
- 스트리밍은 별도의 `cwist-guest-stream` world(0.3 전용)에 있습니다. `host.send-chunk`가 백프레셔와
  함께 청크를 전달하고, `dispatch-stream`은 `_cwist_wasm_dispatch_stream`을 따릅니다. 반영되었고
  스모크 테스트를 마쳤습니다. 아래를 보세요.

## 도구 체인 현황 (2026-09-21 측정)

- WASI 0.3: WASI 하위 그룹이 비준했고, `wasi:cli 0.3.0`이 컴포넌트 모델 비동기를 기본으로 하는
  공식 버전입니다.
- wasi-sdk 34 (2026-08-25): `wasm32-wasip3` sysroot를 포함합니다. CWIST는
  `make wasip2-smoke WASIP2_TARGET=wasm32-wasip3`으로 수정 없이 빌드되고 링크됩니다. wasip3 libc가
  해당 심볼을 다루므로 레시피는 `-lwasi-emulated-pthread`를 자동으로 뺍니다 (p1/p2 sysroot는
  라이브러리를 유지).
- wasmtime 48: 결과 컴포넌트를 실행합니다. 시작, 인메모리 디스패치, 소켓 listen이 모두 동작합니다.
- jco 1.34.0: 0.2와 0.3 컴포넌트를 모두 JS로 변환합니다. 0.3 export는 async 함수로 낮춰지고, WASI
  0.3 import는 `@bytecodealliance/preview3-shim`이 채웁니다 (아래에서 평가).

## 평가 완료: preview3-shim (2026-09-23)

`make component-smoke-p3`는 전체 디스패치 게스트를 0.3 컴포넌트로 실행합니다. 같은 게스트 C
소스를 wasi-sdk 34 이상으로 `wasm32-wasip3`용으로 빌드하고, `wasm-tools component embed`로
컴포넌트화하고, jco 1.34로 변환하고, node에서 preview3-shim 0.6.1로 호스팅한 뒤, 0.2 스모크와
같은 다섯 단언으로 구동합니다. 통과(PASS). 주의 사항은 모두 호스트 쪽입니다.

- node에는 `--experimental-wasm-jspi`가 필요합니다 (JSPI가 canonical ABI 비동기 낮춤을 구동).
  Makefile이 이를 넘기며, 브라우저 플래그 결정은 들어 있지 않습니다.
- jco는 모든 0.3 export를 async 함수로 낮추므로, wasip3 게스트에 대해서는 `handle()`이 Promise를
  반환합니다. `wasm/npm/component.js`가 이를 감지하므로 두 대상 모두에서 `await`가 똑같이
  동작합니다.
- preview3-shim은 완료 후에도 node 이벤트 루프를 살려 두므로, 스모크는 명시적으로 종료합니다.

## 반영 완료: 스트리밍/SSE를 위한 네이티브 비동기 (2026-09-23)

SSE 경로(지금은 Emscripten의 EM_JS 청크 콜백)의 0.3 형태는 wit/cwist.wit의 `cwist-guest-stream`
world입니다. 비동기 호스트 import `host.send-chunk`와 동기 export `dispatch-stream`으로
구성됩니다. `make component-stream-smoke-p3`가 종단 간으로 이를 증명합니다. SSE 라우트
(`/events`)가 `cwist_app_dispatch_stream()`으로 디스패치되고, 직렬화된 각 청크가 JS 호스트의
`sendChunk` 구현으로 넘어가며, 머리가 먼저 오는 청크 순서를 단언합니다.

펌프에는 continuation 재작성이 필요 없었습니다. 청크 싱크가 비동기 import를 시작하고, 즉시
반환되지 않으면 waitable set에서 블로킹합니다. export 작업은 펌프 중간에 멈췄다가 호스트가 호출을
완료하면 재개되므로, `cwist_app_dispatch_stream()`의 동기 `write_fn` 루프가 그대로 실행됩니다.
백프레셔는 호스트가 완료를 늦추는 것에서 자연스럽게 생깁니다. 이것이 반영되기 전 스파이크가
확인한 것: `async func`가 WIT에서 파싱되고 검증된다. wit-bindgen 0.62가 wasi-sdk 34로
wasm32-wasip3용으로 컴파일되는 waitable-set/callback 바인딩을 생성한다. jco는 사용자 정의 호스트
import를 평범한 ESM import로 노출하며, 스모크(와 모든 번들러 소비자)는 그렇게 `sendChunk`를
연결한다.

이 world는 wasm32-wasip3만 빌드합니다. 0.2 컴포넌트는 비동기 import를 낮출 수 없습니다. 바인딩은
world마다 생성되며(`wit-bindgen --world`), `wasm-tools component embed`는 world를 명시적으로
고릅니다 (`--world cwist-guest[-stream]`).

## 해결됨: wasip3에서의 소켓 요청 경로 (2026-09-22 감사)

wasip3 소켓 서버에 대한 첫 HTTP 요청이, 응답 객체는 전송 호출 직전까지 멀쩡한데도
`serialize_headers`(`res->version->data`, http.c:1885)에서 엉뚱한 음수 주소(~`0xffff9c00`)의 범위
밖 읽기로 트랩되곤 했습니다. 감사(wasi-sdk 34, wasmtime 49, `-O1`/`-O2`, 1 KiB/2 KiB부터
8 KiB/16 KiB까지의 버퍼 크기)는 소거법으로 다음을 확립했습니다.

- CWIST 메모리 버그가 아님: 응답 객체는 전송 호출 지점에서 유효하며, 문제의 접근은 p3로 바뀐 소켓
  경로가 실행된 뒤에만 잘못 동작합니다.
- 버퍼 크기 문제가 아님: 16배 범위에서 똑같이 실패합니다.
- 코드 생성에 민감함 (진입 출력 하나로 한 번 사라졌음). 그래서 감사가 형제 호출 제거라는 막다른
  길로 들어갔지만, 그 플래그만으로는 `-O2`가 고쳐지지 않았습니다.
- 근본 원인: **스택 고갈.** p3의 비동기 낮춤이 프레임 구성에 들어오면 wasm-ld의 기본 64 KiB
  스택은 소켓 서빙 체인에 너무 작습니다. `-z stack-size=131072`는 `-O1`과 `-O2`에서 결정론적으로
  통과합니다. Makefile은 모든 wasip2/wasip3 게스트를 `-Wl,-z,stack-size=$(WASIP2_STACK_BYTES)`
  (기본 1 MiB)로 링크하며, 이는 wasip3를 여유 있게 감당합니다.

평범한 BSD 소켓 최소 재현(accept/read/write, CWIST 없음)은 같은 도구 체인에서 그대로 통과하므로
프레임이 작은 게스트는 영향을 받지 않습니다. 기본 한도를 넘는 것은 CWIST의 약 24 KiB
요청/응답 스택 버퍼에 p3 낮춤 오버헤드가 더해진 것입니다.

## 단계

1. **WIT + 검증 (반영 완료).** world 정의, `make wit-check`, `make jco-transpile`, 그리고 이 문서.
2. **jco 브라우저 스파이크 (반영 완료).** `tests/wasm_component_guest.c`는 wit-bindgen의 canonical
   ABI shim으로 cwist-guest world를 export하는 디스패치 게스트입니다
   (`include/cwist/wasm/wasm_component.h`에 공유 도우미가 있음). `make component-smoke`는 이를
   wasm32-wasip2용으로 빌드하고, `wasm-tools component embed`로 컴포넌트화하고, jco로 변환한 뒤,
   `createCwistFromComponent` 어댑터(`wasm/npm/component.js`)를 통해 node에서 Emscripten 래퍼
   테스트와 같은 단언(서명 쿠키 세션 왕복과 dispatch-error 변형 포함)으로 구동합니다. WASI
   import는 `@bytecodealliance/preview2-shim`이 채웁니다. 브라우저 패키징은
   `make component-browser-smoke`가 관문입니다. `--no-nodejs-compat` 재변환과 어댑터가 node 전용
   import 없이 esbuild `--platform=browser`로 번들되어야 하며, 이것이 번들러 소비자에게 필요한
   성질입니다 (번들 자체는 wasm 조각을 fetch로 불러오며 스모크에서는 실행되지 않음). npm 패키지는
   exports 맵(`cwist-wasm/component`)으로 어댑터를 노출합니다. Emscripten은 이와 무관하게 계속
   지원됩니다.
3. **0.3 전환 (조건부).** 두 선행 조건 모두 평가에서 유지되었고, 스트리밍 경계는
   `cwist-guest-stream`에 반영되었습니다. 전환 전에 남은 것: node와 브라우저에서 JSPI가 플래그
   없이 출시되는지 추적하고, SSE 예제를 EM_JS 콜백에서 옮기는 것. 그때가 되어야 Emscripten 빌드가
   CI에서 빠집니다.

## 브라우저 런타임 관문 (WASI 0.2)

`make component-browser-smoke`는 여전히 패키징만 확인합니다. `make component-browser-test`는 그
빌드를 실행한 뒤, Playwright로 헤드리스 Chromium에서 생성된 번들과 실제 WASM 조각을 실행합니다.
먼저 테스트 전용 의존성과 브라우저를 설치하세요.

```sh
npm ci --prefix tests/browser --ignore-scripts --no-audit --no-fund
(cd tests/browser && npx --no-install playwright install --only-shell chromium)
make component-browser-test NODE="$(command -v node)"
```

Linux CI에서는 Playwright 설치 명령에 `--with-deps`를 추가하세요. lockfile이 Playwright와 그에 따른
Chromium 리비전을 고정하며, 둘 다 프로덕션 `cwist-wasm` 패키지에는 추가되지 않습니다. 기존
컴포넌트 선행 조건(wasi-sdk 25, wasm-tools, wit-bindgen, jco)은 그대로 적용됩니다.

러너는 생성된 산출물의 인메모리 허용 목록을 루프백 전용 임시 포트에서 제공합니다. GET `/hello`,
응답 헤더, 그리고 Node 스모크와 같은 NUL 없는 바이너리 페이로드로 POST `/echo`를 확인합니다.
고정물이 C 문자열을 쓰므로 임의의 NUL 바이트를 다루는 것은 아닙니다. 브라우저 단언의 구조화된
결과와 실제 WASM 조각 요청을 요구합니다. 페이지 예외, 로드 실패, 의존성 부재, 타임아웃은 건너뛰지
않고 실패로 처리합니다. 종료 시 브라우저와 서버를 닫습니다.

이 대상은 부정 대조군 두 개도 시작합니다. 사용할 수 없는 WASM 조각과 주입한 브라우저 예외입니다.
각각 자기만의 진단과 함께 종료 코드 1로 끝나야 하며, 크래시, 타임아웃, 브라우저 부재는 기대한
실패로 인정하지 않습니다. 이미 빌드한 산출물로 다시 실행하려면:

```sh
node tests/browser/run.mjs --self-test
```

이 관문은 Chromium에서의 WASI 0.2만 다룹니다. Service Worker 커버리지, WASI 0.3/JSPI 지원,
Emscripten 대체를 주장하지 않습니다.

## 2단계를 만들며 측정한 것

- 처음 작성한 WIT는 wit-bindgen 검증을 통과한 적이 없었습니다. 에러 변형이 패키지 최상위에
  있었기 때문입니다. 이를 `guest` 인터페이스 안으로 옮겼고, 이제 `make wit-check`가 world를 실제로
  검증합니다.
- `wasm-tools component embed`(1.259)는 최종 컴포넌트를 한 단계로 만들어 냅니다. cwist-guest
  world를 wasi-sdk의 component-type 섹션에 합치므로 별도의 `component new` 단계가 필요 없습니다.
- wit-bindgen 0.62는 게스트 export를 bool(ok/err 출력 파라미터)을 반환하는 평범한 C 함수로
  낮춥니다. 생성된 `cabi_post` 훅이 해제하므로 반환하는 `list<u8>`은 libc 할당이어야 합니다.
  `__wasi__`에서 `cwist_alloc`은 libc 기반이므로 디스패치 응답이 그대로 넘어가고, 설계대로 명시적
  해제 진입점이 사라집니다.
- wit/에 world가 두 개 있으면 wit-bindgen과 `wasm-tools component embed` 모두 명시적인
  `--world`가 필요합니다. 바인딩은 .wit-bindings 아래의 world별 디렉터리에 들어가며, 각 게스트는
  자기 component-type 객체를 링크합니다.

## 목표가 아닌 것

- 어느 단계에서도 `cwist-wasm` npm 래퍼의 ABI를 깨지 않습니다.
- 3단계가 끝나기 전에는 Emscripten 빌드를 제거하지 않습니다.
