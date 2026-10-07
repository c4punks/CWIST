# WASI 대상

WASI 0.2(`wasm32-wasip2`)가 지원되는 WASI 종류입니다. 실제 소켓을 바인딩하고 `wasi:sockets`로
HTTP를 제공하며, CI에서 `.github/workflows/wasm.yml`의 `wasip2` 작업(wasi-sdk 25 + wasmtime 25에서
`make wasip2-smoke`)이 관문 역할을 합니다. Preview1(`wasm32-wasi`)은 인메모리 디스패치 범위만
다루며 소켓 런타임이 없습니다. 특이 사항은 CWIST 쪽에서 처리합니다 (아래 참고).

## 종류 감지

`__wasi__`는 두 종류 모두에서 정의되지만 libc 능력이 크게 다르므로, CWIST는 sysroot가 실제로
제공하는 것을 기준으로 기능을 나눕니다 (`include/cwist/sys/wasi.h`).

| | preview1 | wasip2 |
|---|---|---|
| 소켓 런타임 관문 | `CWIST_WASI_NO_SOCKETS` | `CWIST_WASI_SOCKETS` |
| 소켓 | 자리만 있는 `sys/socket.h` | 완전한 `wasi:sockets` 표면 |
| 구분자 | - | `<netdb.h>` (preview1에는 없음) |
| fork / 신호 / rlimit / 스레드 | 없음 | 없음 |
| `getpid` | 없음 | `libwasi-emulated-getpid` |
| `sendmsg(2)` | - | 없음: `send()`로 합침 |
| UDP / TLS | - | UDP는 선택; TLS는 링크되지 않음 (평문) |

fork/신호/rlimit/스레드를 제외하는 가드는 그냥 `__wasi__`에 남고, 소켓 서버 런타임 주변의 가드는
`CWIST_WASI_(NO_)SOCKETS`를 기준으로 합니다.

## WASI 0.2 소켓 서버

빌드, 배포, 재시작 전체 안내는 [Wasmtime 어플라이언스 가이드](../deployment/wasmtime-appliance.md)를
보세요. 예제의 네트워크와 영속성 한계도 설명합니다.

`make wasip2-smoke`는 `libcwist_wasip2.a`를 빌드합니다. 여기에는 `WASM_SRCS`와 함께
`src/sys/wasi/compat.c`, `metrics.c`, `writer_fast.c`, `async.c`, `src/core/log/log.c`,
`src/sys/sys_info.c`, 그리고 서빙 경로가 끌어오는 libttak 단위(`net/lattice.c`,
`net/mols_control.c`, `shared/shared.c`, `timing/deadline.c`, `mem/epoch.c`, `mem/mem.c`,
`mem/fastpath.c`, `mem/owner.c`, `mem/abstract.c`)가 들어갑니다. 그다음 다음 명령으로
`tests/wasip2_smoke.c`를 실행합니다.

```
wasmtime run -S preview2=y -S tcp=y -S inherit-network=y \
    --env CWIST_C1M_MODE=0 wasip2_smoke.wasm
```

스모크 바이너리는 인메모리 디스패치를 확인한 뒤, `cwist_app_listen()`으로 블로킹 accept 루프에서
평문 HTTP를 제공합니다 (단일 스레드 호스트: 워커 풀 없음, epoll 리액터 없음, fork 없음). Makefile
대상은 curl로 이를 확인하고 이후 wasmtime 프로세스를 종료합니다. 측정된 출력:

```
wasip2: in-memory dispatch OK
wasip2: listening on port 18099
wasip2-smoke: PASS (socket server served /hello over wasi:sockets)
```

wasip2에 대한 CWIST 쪽 특이 사항 처리:

- wasi-libc의 소켓 계층에는 `sendmsg`/`recvmsg`가 없습니다 (`wasi:sockets`에는 scatter-gather가
  없음). `cwist_http_sendmsg_all()`과 `cwist_http_sendmsg_speculative()`는 iov를 버퍼 하나로
  합치고 평범한 `send()`를 씁니다.
- C1M 리액터는 epoll/eventfd 기반입니다. `cwist_app_listen()`은 `__wasi__`에서 블로킹 accept
  대체 경로를 강제합니다 (그곳에서는 `CWIST_C1M_MODE`를 무시함).
- TLS 연결 핸들러는 컴파일에서 빠진 채로 남습니다 (BoringSSL 없음). WASI에서 SSL을 요청하면 분명한
  에러와 함께 실패합니다. HTTP/3 UDP, 정적 캐시 감시 스레드, 워커 fork/회수는 꺼져 있습니다
  (스레드/프로세스 없음).
- 대기 쓰기/비동기 경로가 링크 시점에 참조하는 리액터 진입점은 `src/sys/wasi/compat.c`의 평범한 C
  스텁입니다. 그 파일의 metrics와 writer-fast 스텁은 실제 단위가 wasip2 빌드에 들어오므로
  preview1 전용입니다.
- 요청 처리 호출 체인은 스택을 96KB 가까이까지 씁니다. 이는 wasm-ld의 기본값 64KB를 넘으며,
  넘침은 트랩되지 않고 선형 메모리를 조용히 손상시킵니다. wasip2 바이너리는 더 큰 스택으로
  링크하세요 (`-Wl,-z,stack-size=...`, Makefile의 `WASIP2_STACK_BYTES`).

## WASI preview1 (폐기됨)

preview1 대상은 WASI 0.2로 대체되며 제거되었습니다. 0.2 빌드가 같은 인메모리 디스패치 범위를
다루면서 소켓 서버를 더하고, WASM 종류 세 가지는 얻는 것보다 비용이 컸기 때문입니다 (이슈 #203).
아래 내용은 preview1 빌드가 있었을 때의 동작을 기록한 것입니다.

`make wasi-smoke`는 wasi-sdk로 `WASM_SRCS` 부분 집합을 컴파일하고 wasmtime에서 디스패치 + 세션
스모크를 실행했습니다. `WASI_SDK`(기본값 `~/toolchains/wasi-sdk-25.0-x86_64-linux`)와 PATH의
`WASMTIME`, 그리고 `libwasi-emulated-pthread`가 필요했습니다.

### preview1 빌드가 다르게 했던 것

- **libttak**(업스트림 `c4punks/libttak` main): `ttak/compat/pthread.h`는 sysroot 자체의
  `<pthread.h>`가 있으면 그것을 우선하고, 없으면 자체 포함된 단일 스레드 스텁으로 대체합니다.
  `ttak/mem/mem.h`는 `mincore`/`VirtualQuery`가 없는 대상에서 `page_base`를 조용히 처리합니다.
- **소켓 서버 런타임**: `http.c`/`app.c`의 소켓 장치는 `CWIST_WASI_NO_SOCKETS`에서 컴파일에서
  빠집니다. `cwist_app_listen()`은 Emscripten처럼 -1을 반환하고, 호스트는
  `cwist_app_dispatch_memory()`로 요청을 처리합니다.
- **암호**: `session.c`와 `seq_auth.c`는 함께 들어 있는 헤더 전용 SHA-256/HMAC
  (`include/cwist/core/crypto/sha256.h`)을 씁니다. 엔트로피는 `getentropy()`
  (`__wasi_random_get`)에서 옵니다.
- **할당**: `alloc.c`/`arena.c`는 `__wasi__`에서 Emscripten 방식의 평범한 libc 경로를 탑니다.
- **링크 모델**: `-fvisibility=hidden -Wl,--gc-sections -Wl,--allow-undefined
  -lwasi-emulated-pthread`. Emscripten 링크가 시스템 스텁 라이브러리와 binaryen DCE로 같은 서버
  전용 참조를 만족시키는 방식을 그대로 따릅니다.

### 검증 (기록)

`make wasi-smoke` 출력 (wasmtime):

```
WASI dispatch OK (73 bytes)
WASI 404 path handled (rc=-1, no trap)
WASI session secret OK
WASI SMOKE PASS
```

(응답 버퍼 없이 404 rc=-1이 나오는 것은 Emscripten 대상에서도 원래 그런 계약입니다. 없는
라우트는 디스패치 실패가 아닙니다.)
