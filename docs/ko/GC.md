# Full GC: CWIST의 자동 자원 회수

상태: **지원되는 옵트인 기능** (`src/core/mem/gc.c`, `include/cwist/core/mem/gc.h`).
`cwist_full_gc(true)`는 앞으로 생길 기능이 아니라 지금 실제로 호출할 수 있는 스위치입니다.
최소 예제는 튜토리얼 30(`tutorials/30-graceful-shutdown/`)을 보세요. 아래 여섯 개의 설계 절은
5절(투명한 `malloc` 가로채기)을 포함해 모두 구현되고 테스트되었습니다. 이 문서는 이 모드가 다루는
범위와 다루지 않는 범위에 대한 설계 계약으로 남아 있으며, 명시적인 destroy 계열 모델은
full-GC 모드를 켜든 켜지 않든 계속 완전히 지원되고 기본값입니다.

## 동기

지금의 CWIST는 규율 있는 명시적 정리에 의존합니다. `cwist_app_destroy()`, 전송 계층의 연결/세션
정리, 요청 아레나와 짝을 이루는 `cwist_free()`가 그것입니다. 여기에는 두 가지 빈틈이 남아
있습니다.

1. **스레드/프로세스 종료가 연결을 닫지 않습니다.** 명시적 destroy 경로가 실행되지 않은 채 워커
   스레드(또는 프로세스 전체)가 끝나면, 그 연결은 상대방이나 OS가 포기할 때까지 남아 있습니다.
2. **`cwist_alloc`에는 여전히 수동 `cwist_free`가 필요합니다.** 하나라도 빠뜨리면 그 객체는
   소유자가 살아 있는 동안 계속 새어 나갑니다.

Full-GC 모드(`cwist_full_gc(true)`)는 두 빈틈을 모두 메웁니다. 꺼져 있으면(기본값) 현재의 명시적
모델이 비용 없이 그대로 동작합니다.

## 설계

### 1. `cwist_full_gc(bool)` - 전역 스위치

```c
cwist_full_gc(true);   /* enable automatic reclamation */
```

프로세스 전체에 하나뿐인 스위치입니다. **이 스위치는 `cwist_multiport_get_app()`의 포트별 서브
애플리케이션을 가로질러 적용됩니다.** 멀티포트는 분리된 포트를 "독립적으로 조정 가능한 서브
애플리케이션"이라고 소개하지만, 그 독립성은 앱 수준 설정(라우트, 미들웨어, TLS, 크기 제한)에만
해당하며 프로세스 전역 서브시스템에는 처음부터 해당하지 않았습니다. `cwist_full_gc()`가 바로
그런 서브시스템입니다. 여러 `cwist_app` 인스턴스가 한 프로세스를 공유할 수는 있지만(그것이
멀티포트의 핵심입니다) 서로 다른 full-GC 설정을 가질 수는 없습니다. 어느 하나에서 켜면 모두에서
켜지며 앱별로 끌 방법은 없습니다. 스위치, 대기 중인 정리(pending-sweep) 장부, 그리고 이것이
구동하는 에포크 회수 파이프라인이 모두 `cwist_app *`가 아니라 프로세스 범위의 싱글톤이기
때문입니다. (프로세스 시작 시 생성자 함수로 설치되는 cJSON 할당자 훅도 같은 이유로 같은
모양입니다. cJSON 할당자는 앱마다가 아니라 프로세스마다 하나입니다.) 서브 앱마다 다른 메모리 관리
동작이 정말 필요한 배포라면 별도의 프로세스가 필요합니다. 한 프로세스 안의 별도 `cwist_app`
인스턴스로는 그렇게 할 수 없습니다.

켜면 다음과 같이 동작합니다.

- 명시적인 destroy 계열 호출이 없었더라도 **워커 스레드 종료**와 **프로세스 종료** 시 연결이
  자동으로 닫힙니다.
- `cwist_alloc` 객체가 회수 엔진에 등록되어, 수동 `cwist_free` 호출 대신 에포크 순환으로
  해제됩니다.

꺼져 있으면 추적 장치가 하나도 동작하지 않으며, 명시적 모델은 현재의 성능 특성을 유지합니다.

### 2. 회수 엔진: libttak 에포크 GC

엔진은 libttak의 에포크 GC(`ttak_epoch_gc`, `lib/libttak/include/ttak/mem/epoch_gc.h` 참고)이며,
CWIST가 이미 `src/core/mem/gc.c`에서 감싸 두었습니다 (`cwist_gc`, `cwist_reg_ptr`,
`cwist_gc_rotate`).

- 각 워커 스레드는 살아 있는 연결과 `cwist_alloc` 블록을 현재 에포크에 등록합니다.
- 회수는 **에포크 지연** 방식입니다. 정리 대상이 된 객체는, 그 객체를 아직 참조할 수 있는 모든
  스레드를 에포크가 지나갈 때까지 회수되지 않습니다. 따라서 스레드 종료 시 닫힌 연결이, 그 연결로
  요청을 처리하는 중인 스레드에서 use-after-free를 일으키는 일은 없습니다.
- 순환은 자동(할당 힌트에 따라 주기를 조절하는 백그라운드 순환 스레드)이거나 수동(이벤트
  루프에서 `cwist_gc_rotate` 호출)일 수 있으며, 기존 래퍼의 의미와 같습니다.

### 3. 스레드/프로세스 종료 정리

- **스레드 종료**: 스레드별 연결 레지스트리가 스레드 로컬 저장소에 있고, pthread TLS 소멸자가
  스레드 종료 시 그 스레드가 아직 소유한 것을 정리하고 닫습니다.
- **프로세스 종료**: `atexit` 정리가 프로세스 전체에 대해 같은 마무리를 수행하므로, 남은 연결은
  버려지지 않고 정상적으로 종료됩니다 (TLS 종료, 소켓 닫기, 세션 정리).

### 4. 핸들 같은 지역 변수를 위한 의사 RAII

스택 범위의 핸들을 위해 CWIST는 의사 RAII 가드를 제공합니다.

- 변수가 범위를 벗어날 때 대응하는 정리를 실행하는 GCC/Clang
  `__attribute__((cleanup))` 범위 가드. C에서 RAII에 해당하는 실용적인 방법입니다.
- cleanup 속성을 쓸 수 없는 곳에서는 LibTTAK RAII 기본 요소를 그대로 빌려 씁니다.

### 5. 투명한 `malloc` 가로채기

사용자는 습관적으로 `cwist_alloc`이 아니라 `malloc`을 씁니다. 손가락의 규율에 기대는 것이
누수 없음 주장이 무너지는 지점입니다. `<cwist/core/mem/intercept.h>`는 번역 단위별 옵트인으로
두 표기를 동등하게 만듭니다.

```c
#define CWIST_INTERCEPT_MALLOC
#include <cwist/core/mem/intercept.h>
#include <cwist/app.h>

static void handle(cwist_http_request *req, cwist_http_response *res) {
    char *buf = malloc(256);   /* -> cwist_malloc_shim() */
    ...
    /* no free(buf) needed: under cwist_full_gc(true), this evaporates
     * exactly like a forgotten cwist_alloc() would. free(buf), if called,
     * still works normally either way. */
}
```

- **cJSON 자체의 내부 할당**은 `cJSON_InitHooks`를 통해 별도로 `cwist_alloc`으로 돌려집니다
  (`src/core/mem/alloc.c`). 아래 shim이 생기기 전부터 그랬고 옵트인도 필요 없습니다.
- **핸들러 자신의 `malloc`/`calloc`/`realloc`/`free`**는 번역 단위가 헤더를 포함하기 전에
  `CWIST_INTERCEPT_MALLOC`을 정의하면 `cwist_malloc_shim`/`cwist_calloc_shim`/
  `cwist_realloc_shim`/`cwist_free_shim`(`src/core/mem/alloc.c`)으로 돌려집니다.
  `malloc`/`calloc`은 원래 의미(각각 초기화되지 않은 메모리와 0으로 채운 메모리. 어느 쪽도
  항상 0으로 채우는 `cwist_alloc`의 동작이 되지 않습니다)를 유지하며, `cwist_full_gc_enabled()`일
  때만 pending-sweep 목록에 등록됩니다. `realloc`은 들어온 포인터가 추적되던 것인지에 따라 추적을
  유지하거나 버리며, 추적되지 않던 포인터를 스스로 추적 대상으로 만들지는 않습니다. `free`는
  먼저 추적을 해제하고(추적된 적 없는 포인터라면 안전한 무동작) 실제 `free`를 호출합니다.

**왜 링크 수준 `-Wl,--wrap=malloc`이 아니라 헤더 범위인가**: 링크 수준 래핑은 최종 바이너리의
*모든* `malloc` 참조를 가로채며, 여기에는 CWIST 헤더를 전혀 보지 않고 아래에 libc가 아닌
할당자가 있으리라 예상할 이유도 없는 내장 의존성(BoringSSL, lsquic, cnats, sqlite3)도
포함됩니다. 이는 가정만의 위험이 아닙니다. BoringSSL은 평범한 힙 의미를 전제로 비밀 키 자료를
해제하기 전에 `OPENSSL_cleanse()`로 0으로 덮어씁니다. 그 할당을 에포크 지연 GC 아레나로
돌리면 cleanse 후 free가 메모리를 회수 가능 상태가 되기 전에 확실히 지운다는 보장이 사라집니다.
이것은 성능상의 각주가 아니라 보안 퇴행입니다. 헤더 범위의 `#define`은 명시적으로 옵트인한
번역 단위에서만 효과가 있고, 내장 의존성은 구조적으로 절대 옵트인하지 않으므로(CWIST 헤더를 아예
포함하지 않습니다) 설계상 제외됩니다. 또한 헤더는 매크로를 정의하기 전에 `<stdlib.h>`를 먼저
포함합니다. 그래야 이후의 `#include <stdlib.h>`(이 번역 단위의 것이든 다른 헤더를 거친 것이든)가
include 가드에 걸려 아무 일도 하지 않습니다. 그렇지 않으면 이미 활성화된 매크로를 통해
`malloc`/`calloc`/`realloc`/`free` 선언이 다시 전개되어 확실하게 컴파일이 깨집니다.

**회수 주기**는 기존의 `cwist_gc_scope_track`/`cwist_gc_scope_flush` 파이프라인을 그대로 다시
씁니다 (스레드별 추적, 최후의 수단인 스레드 종료 TLS 정리, `src/sys/io/io_queue.c`에 이미 연결된
작업별 flush 같은 자연스러운 완료 지점에서의 명시적 flush). 별도의 정책이 아니라
`cwist_alloc()`이 이미 쓰는 메커니즘과 같습니다. 스레드 종료 시에만 회수하는 정책은 의도적으로
오래 사는 C1M 리액터 워커 스레드에서는 거의 아무 일도 하지 않을 것이고, 바로 그 배포 모델이 이
기능이 가장 도움이 되어야 하는 곳입니다.

**스레드 간 인계**는 기존의 `cwist_gc_scope_disown()` 탈출구를 그대로 씁니다 (아래와
`tests/test_full_gc_ownership_handoff.c` 참고). 할당한 범위보다 오래 살아야 하는 포인터(연결
풀에 캐시하거나 백그라운드 작업에 넘기는 경우)는 인계 지점에서 이 함수를 한 번 호출하면 새
소유자의 책임이 됩니다. shim으로 할당한 포인터를 위한 별도 API는 없으며 `cwist_alloc()` 포인터와
같은 API를 씁니다.

**측정된 오버헤드** (`tests/bench_malloc_intercept.c`, 단일 스레드, 가끔 `realloc`이 섞인
16-512바이트 할당; GitHub Actions `ubuntu-latest`, AMD EPYC 7763, 3회 실행 중앙값):

| 설정 | ns/op | 기준 대비 |
|---|---:|---:|
| 기준 (shim 없음) | 16.7 | - |
| shim, full-GC 끔 (기본값) | 17.6 | +5% |
| shim, full-GC 켬 | 24.3 | +46% |

"끔" 비용은 연산마다 relaxed atomic load 한 번(`cwist_full_gc_enabled()`)과 함수 호출 하나가 더
드는 것으로, `cwist_alloc()`/`cwist_free()`가 이미 내는 비용과 같습니다. "켬" 비용은 안전망
자체의 비용으로, `malloc`/`free` 한 쌍마다 호출 스레드의 pending 집합에 한 번 넣고 한 번 빼는
것입니다. 살아 있는 블록이 많고 스레드가 여럿일 때 이 비용이 어떻게 되는지는 아래 "알려진 성능
주의 사항"을 보세요.

### 6. `cwist_alloc` 내부

`src/core/mem/alloc.c`에 등록 훅이 추가됩니다. full-GC 모드에서는 모든 `cwist_alloc` 블록이
에포크 GC에 기록되므로(`cwist_reg_ptr_sized`로 크기 인식) 에포크 순환이 그것을 회수합니다.
명시적 `cwist_free`도 여전히 올바르며, 단지 블록의 등록을 일찍 해제할 뿐입니다.

### 7. 명시적 flush와 회수

쉽게 가정하기 쉽지만 둘 다 틀린 내용이 두 가지 있습니다.

- **"retire"는 "해제"가 아닙니다.** `cwist_gc_scope_flush()`와 `cwist_ebr_free()`는
  블록을 *retire*할 뿐입니다. 실제 해제는 누군가 회수를 구동할 때 일어납니다.
  `cwist_gc_pipeline_tick()`이나 `cwist_scheduler` 뒤의 io_queue(`cwist_app_use_scheduler()`,
  `cwist_async_set_timeout()`)가 그 역할을 합니다. full-GC 인스턴스는 수동 순환이라
  저절로 회수가 돌지 않습니다. 회수를 구동하지 않는 프로세스는 flush한 것을 전부 그대로
  들고 있고, retire할 때마다 돌려받지 못하는 노드도 하나씩 할당됩니다.
- **에포크 지연은 에포크 안에 있는 읽기 스레드만 보호합니다.** 요청 처리 스레드(HTTPS pool,
  리액터, h2 루프)는 `ttak_epoch_enter()`를 호출하지 않습니다. 회수가 한 번 돌면, 그 스레드가
  아직 쓰고 있는 블록이라도 retire된 블록은 해제됩니다.

그래서 `cwist_gc_scope_flush()`에는 엄격한 전제 조건이 있습니다. **호출 스레드가 앞으로 다시
건드릴 추적 블록을 하나도 들고 있지 않아야 합니다.**

| 호출 위치 | 괜찮은가? | 이유 |
|---|---|---|
| 전용 워커 스레드에서 자기 완결적인 작업이 끝난 뒤, 결과를 `cwist_gc_scope_disown()`이나 `cwist_async_respond_with()` 같은 disown하는 API로 넘긴 다음 | 예 | 작업이 할당한 것 중 살아 있는 게 없음 |
| 요청 핸들러나 미들웨어 (`next()`가 돌아온 뒤라도) | **아니요** | 응답은 그 뒤에도 읽히고 전송되며(압축, 직렬화), `cwist_http_header_add()`로 추가한 헤더는 추적 대상임 |
| 연결을 처리하는 모든 스레드 | **아니요** | 연결별 상태(h2 세션 버퍼, HPACK 테이블)가 요청 사이에도 그 스레드에 살아 있음 |

`cwist_conn_registry_flush()`도 같은 구조입니다. 호출 스레드가 등록한 모든 연결을 닫는데,
지금 다른 스레드가 처리 중인 연결(park, defer, 재제출된 연결)도 포함됩니다.

`cwist` CLI는 이 내용을 읽도록 강제합니다. `cwist audit`와 `cwist watcher`는 애플리케이션
코드에서 `cwist_gc_scope_flush`, `cwist_ebr_free`, `cwist_gc_scope_disown`/`cwist_gc_scope_untrack`,
`cwist_conn_registry_flush`, `cwist_conn_registry_sweep_all`을 호출하는 모든 곳에 경고를 냅니다.
경고마다 해당 API의 계약이 적혀 있습니다. 호출 위치가 계약을 지키는지 확인했다면, 그 위치에
표시를 달아 경고를 끄세요.

```c
/* 작업 끝: 이 작업이 할당한 것은 위에서 모두 해제하거나 disown했음. */
cwist_gc_scope_flush(); /* cwist-ack: cwist_gc_scope_flush */
```

(표시는 호출 바로 윗줄에 달아도 됩니다.) 이 내용은 c4punks/CWIST#347에서 나왔습니다. 한
애플리케이션이 요청 미들웨어에서 flush를 하고 있었는데, 그 프로세스에서 회수를 구동하는 곳이
하나도 없었기 때문에 문제가 드러나지 않았을 뿐입니다.

## 의미 요약

| 이벤트 | full_gc 없음 (기본값) | `cwist_full_gc(true)` |
|---|---|---|
| 워커 스레드 종료 | 연결을 명시적으로 닫아야 함 | TLS 정리가 소유한 연결을 닫음 |
| 프로세스 종료 | `cwist_app_destroy()` 필요 | `atexit` 정리가 남은 연결을 닫음 |
| `cwist_alloc` 객체 | 수동 `cwist_free` | 에포크 순환이 회수; 명시적 free도 문제없음 |
| cJSON 내부 `malloc` | 해당 없음 (cJSON이 자체 메모리 관리) | `cJSON_InitHooks`로 `cwist_alloc`에 연결; 에포크 순환으로 해제 |
| 핸들러가 맨 `malloc()` 호출 (`CWIST_INTERCEPT_MALLOC` 옵트인) | 잊으면 힙 누수 | pending-sweep 목록으로 연결; 에포크 순환으로 해제 |
| 정리 안전성 | 호출자의 규율 | `ttak_epoch_enter()` 안의 읽기 스레드에 한해 에포크 지연; 요청 처리 스레드는 해당하지 않으므로 명시적 flush는 7절을 따라야 함 |

## 목표가 아닌 것과 참고 사항

- Full-GC 모드는 **옵트인**입니다. 기본 빌드는 추적 오버헤드가 없는 명시적 모델을 유지합니다.
- 이것은 추적(tracing) 가비지 컬렉터가 아닙니다. 회수는 에포크 기반이며 프레임워크가 관리하는
  자원(연결, `cwist_alloc` 블록, 핸들러 컨텍스트의 `malloc`)으로 범위가 정해져 있습니다.
- 커널 수준 자원(파일 디스크립터, TLS 세션)은 종료 정리에서 결정적으로 닫히며, 에포크 지연은
  그 뒤의 메모리 회수에만 적용됩니다.

## 알려진 성능 주의 사항

`cwist_full_gc(true)`에서는 모든 `cwist_alloc()`이 블록을 호출 스레드의 pending 집합에 넣고,
모든 `cwist_free()`가 그것을 뺍니다 (`cwist_strdup()` 결과처럼 추적된 적 없는 블록이라면 찾아보고
실패합니다). 이 집합은 개방 주소법 해시 테이블이므로, 스레드가 블록을 몇 개 가지고 있든 두 연산
모두 평균 O(1)이고, 스레드의 집합은 스레드 로컬 load 한 번으로 찾습니다. 스레드마다 자기 집합이
있고 공유 락은 없습니다.

`tests/bench_full_gc_tracking.c`(`make bench_full_gc_tracking`)는 스레드에 추적 블록 N개를
살려 둔 채로 이를 측정합니다. GitHub Actions `ubuntu-latest` (AMD EPYC 7763, vCPU 4개), 측정당
200000쌍, 3회 실행 중앙값, `cwist_alloc()` + `cwist_free()` 한 쌍당 ns:

| 살아 있는 추적 블록 | full-GC 끔 | full-GC 켬 |
|---|---:|---:|
| 0 | 31.2 | 38.6 |
| 64 | 31.3 | 40.7 |
| 1024 | 19.2 | 32.2 |
| 16384 | 19.2 | 34.1 |

`cwist_strdup()` + `cwist_free()` 한 쌍(free의 조회가 실패하는 경우)은 full-GC를 켜면
22.4-23.5 ns, 끄면 16.6-17.2 ns이며, 같은 살아 있는 집합 크기 전반에서 역시 일정합니다. 스레드당
살아 있는 블록이 1024개일 때, 1/2/4/8개 스레드가 동시에 할당을 반복하면 스레드당 비용은
full-GC를 켰을 때 34.0/33.2/65.5/104.1 ns, 껐을 때 25.6/24.9/49.6/79.8 ns입니다. 스레드가 둘을
넘을 때의 증가는 full-GC를 껐을 때도 나타나므로(할당자, 그리고 vCPU 4개에 스레드 8개),
pending 집합 때문이 아닙니다.

이슈 #65 이전에는 이 집합이 제거할 때마다 훑어보는 리스트였기 때문에, free 한 번의 비용이
스레드가 가진 블록 수에 비례했습니다. 같은 러너에서 살아 있는 블록 1024개일 때 한 쌍에
288.6 ns, 16384개일 때 3853.9 ns였습니다.

**실용적인 지침**:

- full-GC 모드는 수동 `cwist_free()` 관리가 걸림돌이 되는 **빠른 프로토타이핑**과 편의성
  우선 코드에 쓰세요. 실험, 내부 도구, 처리량이 최우선이 아닌 작업에서 안전한 기본값입니다.
- 할당 오버헤드가 나노초 단위로 중요한 **고처리량 프로덕션 서비스**에는 **기본 명시적 모드**를
  유지하세요. 명시적 `cwist_alloc()` / `cwist_free()` 모델은 추적 비용이 없으며 계속 완전히
  지원됩니다.
- 고처리량 서비스에서 full-GC 모드를 켠다면 먼저 할당 핫 패스를 프로파일링하세요. 오버헤드는
  `cwist_full_gc(true)`를 호출했을 때만 생기며, 모든 기본 빌드(full-GC 끔)에는 영향이 없습니다.
