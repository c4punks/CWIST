# 런타임 설정

프레임워크가 읽는 `CWIST_*` 환경 변수를 모두 여기에 적습니다.
`scripts/ci/check_env_knobs.py`(`make env-check`, CI에서 실행)는 `src/`가
이 문서에 없는 변수를 읽거나, 이 문서에 있는 변수를 `src/`가 더 이상 읽지
않으면 실패합니다. 그래서 표가 빠짐없이 유지됩니다.

대부분의 변수는 처음 쓸 때 한 번 읽고 저장해 둡니다. 서비스 중에 바꾸지 말고
프로세스 시작 전(또는 `cwist_app_listen()` 전)에 설정하세요. 비어 있거나
형식이 잘못된 값은, 행에 따로 적지 않은 한 기본값으로 돌아갑니다.

## 상태

변수마다 상태가 하나씩 있습니다. 상태는 v4가 그 변수에 대해 무엇을 약속하는지
나타냅니다.

| 상태 | v4 시리즈 동안의 약속 |
|------|----------------------|
| `supported` | 이름과 의미를 유지합니다. v5 전에는 제거하거나 다른 뜻으로 바꾸지 않으며, `v4` 태그 이후 사라지면 CI 검사가 실패합니다. |
| `tuning` | 이름은 유지하지만, 엔진이 바뀌면서 효과와 기본값은 마이너 릴리스 사이에 바뀔 수 있습니다. 값에 의존하기 전에 측정하세요. |
| `diagnostic` | 디버깅 전용입니다. 출력 형식과 동작은 안정적이지 않으며 어느 릴리스에서든 바뀌거나 없어질 수 있습니다. |

## 프로세스와 서버 모드

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_WORKERS` | supported | 정수 | `1` | 이벤트 루프 전에 fork하는 워커 프로세스 수. |
| `CWIST_C1M_MODE` | supported | `0` 또는 `false`면 끔 | `1` | `1`은 CWIST reactor(C1M)로 서비스하고, `0`은 classic 스레드 풀을 고릅니다. |
| `CWIST_PROFILE` | supported | `default`, `performance`, `lowmem`, `lowlat` | `default` | 환경에 설정되지 않은 다른 변수를 채우는 프리셋입니다(명시한 값이 우선). `default`: `CWIST_C1M_MODE=1`, `CWIST_REACTOR_DRAIN_CHUNK=8`. `performance`: 여기에 `CWIST_MALLOC_ARENA_MAX=1`, `CWIST_HTTP_BATCH=64` 추가. `lowmem`: `CWIST_C1M_MODE=1`, `CWIST_MALLOC_ARENA_MAX=1`, `CWIST_WORKERS=2`. `lowlat`: `CWIST_C1M_MODE=0`. 모르는 값이면 경고하고 `default`를 씁니다. |
| `CWIST_DRAIN_TIMEOUT` | supported | 초, `0`–`3600` | `5` | 종료 시 앱을 해제하기 전에 처리 중인 요청을 기다리는 시간. 범위 밖 값은 stderr에 알리고 무시합니다. |
| `CWIST_FD_LIMIT_TARGET` | supported | 0보다 큰 정수 | `1050000` | 서버가 시작할 때 스스로 올리는 열린 파일 soft limit(hard limit까지). [C1M 파일 한도](c1m-file-limits.md) 참고. |
| `CWIST_MALLOC_ARENA_MAX` | tuning | 0 이상 정수 | 설정 안 함(glibc 기본값) | glibc 전용: `mallopt(M_ARENA_MAX)`. `1`은 할당기 동시성을 조금 내주고 RSS를 크게 줄입니다. |
| `CWIST_MALLOC_TRIM_IDLE_SEC` | tuning | 초, `0`–`86400` | `30` | glibc 전용(그 외 libc에서는 동작 없음): 이벤트 없이 유휴 상태가 누적된 시간이 이 값에 도달하면 리액터가 이벤트 없는 wake에서 `malloc_trim(0)`을 호출해 glibc 아레나 상단의 여유 메모리를 OS에 반환합니다. 유휴 wake에서만 동작해 핫 패스 비용은 없습니다. 로드 라운드 후 워커당 중간값 약 1.2 MiB 절감. `0`이면 끕니다. |

## Classic 스레드 풀

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_WORKER_THREADS` | supported | 0보다 큰 정수 | CPU 코어 수 | 풀의 기본 스레드 수. |
| `CWIST_POOL_PREWARM` | supported | 정수 | 기본 풀 크기 | 기본 풀보다 클 때 시작 시 미리 만드는 스레드 수(풀 최대치까지). 연결이 몰릴 때 스레드 생성을 피합니다. |
| `CWIST_POOL_IDLE_TIMEOUT_MS` | supported | 밀리초, 0 이상 | `2000` | 기본 풀을 넘는 스레드가 놀다가 종료하기까지의 시간. `0`이면 남는 스레드를 계속 유지합니다. |

## HTTP/1.1

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_HTTP_KEEP_ALIVE_TIMEOUT` | supported | 초, 0보다 큼 | `15` | keep-alive 연결을 닫기 전 유휴 시간. |
| `CWIST_ASYNC_STASH_IDLE_SEC` | tuning | 초, 0 이상 | `10` | C1M async HTTP: keep-alive 연결의 큰 rbuf/obuf stash를 re-arm 시 4 KiB 바닥으로 축소하기 전에 용량이 필요 없었던 시간. 바쁜 연결은 용량을 유지해 요청마다 일어나는 축소→재확장 realloc을 없애고, 유휴 연결은 이 시간만큼 늦게 바닥에 수렴합니다. `0`이면 기존처럼 즉시 축소합니다. |
| `CWIST_HTTP_BATCH` | tuning | `1`–`1024` | `16` | 이벤트 루프 한 턴에 디스패치하는 파이프라인 요청 수. 나머지는 다음 턴에 이어집니다. |
| `CWIST_HTTP_YIELD_BATCH` | tuning | `1`–`1024` | `CWIST_HTTP_BATCH` | 연결이 reactor 큐로 양보하기 전에 한 배치 안에서 처리하는 요청 수. |
| `CWIST_USE_TCP_CORK` | tuning | 0보다 큰 정수 | 꺼짐 | Linux, 평문 HTTP/1.1: 소켓을 cork해서 헤더, 본문, 파일 바이트를 꽉 찬 세그먼트로 보냅니다. |
| `CWIST_TCP_CORK_BURST` | tuning | 바이트, 16384 이상 | `262144` | `CWIST_USE_TCP_CORK`를 켰을 때 파일 본문을 한꺼번에가 아니라 이 바이트마다 내보냅니다. |

## HTTPS

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_HTTPS_IDLE_TIMEOUT_MS` | supported | 밀리초, 0보다 큼 | `30000` | HTTPS 연결을 닫기 전 유휴 시간. |
| `CWIST_HTTPS_PARK` | tuning | `0`/`false`면 끔 | 켜짐 | 유휴 TLS 연결이 풀 스레드를 붙잡지 않고 reactor에 대기합니다. |
| `CWIST_HTTPS_HS_SHARDS` | tuning | `1`–`16` | CPU 코어 수, `4`–`16`으로 제한 | 핸드셰이크 샤드 수. 연결 폭주 튜닝용. |
| `CWIST_TLS_SESS_CACHE_SIZE` | tuning | 정수, `0`–`1048576` | `1024` | BoringSSL 서버측 세션 캐시 상한(`SSL_CTX_sess_set_cache_size`)으로, 모든 서버 TLS 컨텍스트(HTTP/1.1, HTTP/2, HTTP/3)에 적용됩니다. 워커 프로세스당 TLS 1.2 세션 ID 보유량을 제한합니다. `0`이면 BoringSSL 기본값(제한 없음)을 씁니다. 세션 삽입 시에만 참조합니다. |

## HTTP/2

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_HTTP2_IDLE_TIMEOUT_MS` | supported | 밀리초, 0보다 큼 | `300000` | HTTP/2 연결을 닫기 전 유휴 시간. |
| `CWIST_HTTP2_GOAWAY_GRACE_MS` | supported | 밀리초, 0 이상 | `2000` | GOAWAY를 보낸 뒤 연결을 닫기까지의 시간. 클라이언트가 이미 시작한 스트림을 끝낼 수 있게 합니다. |
| `CWIST_HTTP2_MAX_RST_BURST` | supported | 0보다 큰 정수 | `100` | 연결을 닫기 전에 클라이언트가 한꺼번에 보낼 수 있는 스트림 리셋 수(rapid reset 방어, CVE-2023-44487). |
| `CWIST_HTTP2_MAX_RST_RATE` | supported | 초당 리셋 수, 0보다 큼 | `100` | 그 리셋 예산이 다시 차는 속도. |
| `CWIST_H2_BATCH_BYTES` | tuning | 바이트, 4096 이상 | `65536` | 보낼 프레임을 모았다가 이만큼 쌓이면(또는 블로킹 대기 전에) 한 번에 씁니다. |

## HTTP/3

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_H3_NO_GSO` | tuning | `1` | 꺼짐 | Linux: QUIC 데이터그램을 UDP GSO로 묶지 않고 하나씩 보냅니다. |
| `CWIST_H3_DEBUG` | diagnostic | 설정 | 설정 안 함 | lsquic 자체 로그를 stderr로 보냅니다(CONNECTION_CLOSE 코드 등). 서버 루프가 시작할 때 읽습니다. |

## WebSocket

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC` | supported | 초, 0 이상 | `300` | async(reactor) WebSocket을 닫기 전 유휴 시간. `0`이면 유휴 소켓을 닫지 않습니다. |

## Reactor

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_REACTOR_BACKEND` | tuning | `epoll` | io_uring | Linux: A/B 측정용으로 epoll 백엔드를 강제합니다. |
| `CWIST_RX_URING` | tuning | `0`이면 끔 | 켜짐 | POLL + `recv` 대신 `IORING_OP_RECV`로 받습니다. |
| `CWIST_ACCEPT_URING` | tuning | `0`이면 끔 | 켜짐 | one-shot POLL + `accept4` 반복 대신 multishot `IORING_OP_ACCEPT` 하나로 새 연결을 받습니다. multishot accept를 지원하지 않는 커널은 조용히 poll 경로로 돌아갑니다. |
| `CWIST_RX_EPOLL` | tuning | `0`이면 끔 | 켜짐 | epoll reactor(io_uring ring이 없거나 `CWIST_REACTOR_BACKEND=epoll`)에서 keep-alive 연결을 edge-triggered로 등록해 둔 채 reactor가 직접 받습니다. 요청마다 `epoll_ctl`로 one-shot poll을 다시 거는 대신입니다. |
| `CWIST_REACTOR_DRAIN_CHUNK` | tuning | 0 이상 정수 | `8` (`CWIST_PROFILE`이 설정, 없으면 `64`) | 게시된 작업을 비우는 사이에 처리하는 연결 콜백 수. 모든 reactor 백엔드(io_uring, epoll, kqueue)에 적용됩니다. `0`이면 묶음 중간의 비우기를 끕니다(기존 동작). [협력적 큐잉](cooperative-queuing.md) 참고. |
| `CWIST_REACTOR_ROUND_BUDGET_US` | tuning | 마이크로초, 0 이상 | `2000` | reactor 라운드가 이 시간을 넘기면, 대기 중인 re-arm을 라운드 끝이 아니라 콜백 64개마다 커널에 넘깁니다. 라운드는 끝까지 실행됩니다. `0`이면 한도를 끕니다. |
| `CWIST_REACTOR_CQ_GRACE_NS` | tuning | 나노초, 0 이상 | `20000` | 커널에서 잠들기 전에 completion ring을 확인하며 도는 시간. `0`이면 돌지 않습니다. |

## 진단

| 변수 | 상태 | 값 | 기본값 | 효과 |
|------|------|----|--------|------|
| `CWIST_ASYNC_DEBUG` | diagnostic | 설정 | 설정 안 함 | C1M async 경로와 reactor의 드문 실패(rearm, submit, SQ 가득 참)를 stderr에 기록합니다. 정상 동작에서는 출력이 없습니다. |
| `CWIST_ASYNC_STRICT` | diagnostic | `1` | 설정 안 함 | 지연 응답 오용 시 경고 한 번 대신 abort합니다(core를 남김). |
| `CWIST_LATENCY_PROBE` | diagnostic | `1` | 설정 안 함 | reactor마다 arm-to-dispatch와 콜백 시간 히스토그램을 기록하고, reactor가 해제될 때 stderr에 출력합니다(issue #166). |
