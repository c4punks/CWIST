# CWIST bare-metal STM32

CWIST는 소켓 없이 `cwist_app_dispatch_memory()` 또는 raw-lwIP 어댑터를 구동하는
Cortex-M 경로를 제공합니다. 호스트 빌드와 같은 라우터, 미들웨어, 핸들러,
JSON 도구, HTTP/1.1 파서를 사용하지만 `cwist_app_listen()`, pthread, epoll,
C1M 리액터는 이식하지 않습니다.

## 지원 대상과 검증 범위

- **참조 통합:** NUCLEO-H743ZI2(STM32H743, Cortex-M7, 2 MB flash, 1 MB SRAM,
  RMII Ethernet, TRNG). `example/stm32/`에 애플리케이션, HAL 포트 훅, lwIP
  옵션, CubeMX 통합 및 플래싱 방법이 있습니다.
- **CI 대상:** `arm-none-eabi-gcc`로 빌드한 Cortex-M7 Thumb-2 펌웨어를
  `qemu-arm` semihosting으로 실행합니다. 명령어와 HTTP/lwIP/TLS 동작을 검증하지만
  실제 보드의 NVIC, PHY, 클럭 트리, RNG는 검증하지 않습니다.
- **메모리 후보:** STM32H7을 우선으로 하고, 충분한 SRAM을 갖춘 F767/F429급을
  검토할 수 있습니다. Cortex-M4F와 F429/F767은 아직 검증된 대상이 아닙니다.
  실제 보드 수용 테스트는 수동 단계이며 QEMU 성공을 보드 성공으로 간주하면 안 됩니다.

F0/F1/L0 및 약 128 KB 미만 SRAM 부품은 범위 밖입니다. 작은 부품은 plain HTTP부터
시작하고, HTTPS는 H7급 메모리 예산을 권장합니다.

## 펌웨어 입력 빌드

ARM EABI GCC와 newlib-nano를 설치한 후 실행합니다.

```sh
make CROSS_COMPILE=arm-none-eabi- CWIST_PLATFORM=baremetal CWIST_TINY=1 amalgamation fat-dist
make baremetal-dist-check
```

산출물은 다음 두 가지입니다.

- `dist/amalgamation/cwist.c`와 `dist/amalgamation/include/`: CubeIDE/CMake에 넣는
  단일 C 파일과 헤더
- `dist/cwist-fat/lib/libcwist_baremetal.a`와 `dist/cwist-fat/include/`: Cortex-M
  아카이브와 헤더

`baremetal-dist-check`는 별도 출력 디렉터리에서 빌드를 반복하고, 생성한
amalgamation을 크로스 컴파일하며, 아카이브 내용을 검사합니다. 두 라이브러리
형태를 한 펌웨어에 함께 넣지 마십시오.

기존 저수준 타깃도 사용할 수 있습니다.

```sh
make baremetal             # BAREMETAL_CROSS로 libcwist_baremetal.a 생성
make baremetal-smoke       # qemu-arm 펌웨어 4종 실행
make baremetal-size-gate   # smoke + 크기/할당 예산 검사
```

`BAREMETAL_CROSS`와 `BAREMETAL_ARCH`로 다른 접두사 또는 Cortex-M CPU/FPU
프로파일을 선택할 수 있습니다. 서로 다른 ARM ABI로 만든 객체는 섞지 마십시오.

## 포트 훅과 런타임 계약

펌웨어는 `cwist/sys/port.h`의 링크 타임 심벌을 제공해야 합니다.

```c
uint64_t cwist_port_monotonic_ns(void);
int cwist_port_entropy(void *buf, size_t len);
```

`cwist_port_entropy()`는 **필수**입니다. MCU의 RNG 주변장치를 읽고 실패 시 0이
아닌 값을 반환해야 합니다. 상수 시드나 클럭+`rand()` 대체는 허용하지 않습니다.
해시 플러딩 방어, 세션 ID, TLS가 이 엔트로피에 의존합니다.

단조 시간 훅은 보통 SysTick, DWT 또는 타이머를 사용합니다. 로깅은 newlib의
`_write`를 통해 동기적으로 이루어지며 예제는 USART3로 별도 스레드 없이 출력합니다.
백그라운드 로거, 스케줄러, GC 스레드, RTOS 락 훅은 없습니다. lwIP 어댑터 호출과
콜백은 모두 lwIP 코어 컨텍스트에서 실행해야 합니다.

RTC가 있으면 newlib의 `time()`/`_gettimeofday`를 제공하고, 없으면 CWIST가 RFC
9110에 따라 HTTP `Date` 헤더를 생략합니다. 정적 자산은 파일 시스템 대신 기존
컴파일 내장 자산 테이블을 사용합니다.

## raw lwIP HTTP

`src/sys/port/lwip.c`를 펌웨어의 `lwipopts.h`와 함께 컴파일하고 lwIP 초기화 후
코어 컨텍스트에서 서버를 만듭니다.

```c
cwist_lwip_config cfg;
cwist_lwip_config_init(&cfg);
cfg.max_conns = 4;
cfg.max_header_bytes = 4096;
cfg.max_body_bytes = 1024;
cwist_lwip_server *server = cwist_lwip_listen(app, &cfg);
```

Content-Length 프레이밍, keep-alive, 순서가 보장된 파이프라이닝,
`Expect: 100-continue`, HEAD, idle timeout, 연결 수 제한, 400/413/431 응답을
지원합니다. Transfer-Encoding 요청 본문은 501을 반환합니다. 연결마다 한 번에
하나의 요청-응답을 처리하며 `tcp_write`, `tcp_sent`, `tcp_poll`로 back-pressure를
적용합니다. CubeMX 설정과 메인 루프는 `example/stm32/README.md`를 참고하십시오.

## 메모리 크기

아래 수치는 loopback 클라이언트와 테스트 버퍼를 포함한 **전체 테스트 펌웨어**
기준입니다. CWIST 단독 비용이 아니라 상한 예산으로 해석해야 합니다. 최신 예산과
측정값은 `scripts/ci/baremetal-budget.txt`에 기록합니다.

| 펌웨어 | Flash | 정적 RAM | Heap 최고치 | Stack | 요청당 할당 |
|---|---:|---:|---:|---:|---:|
| hello | 45,156 B | 1,840 B | 23,308 B | 8,352 B | 1 |
| hello-rtc | 45,132 B | 1,852 B | 23,312 B | 8,800 B | 1 |
| hello-lwip | 75,328 B | 62,432 B | 71,212 B | 8,316 B | 0 |
| hello-tls | 138,308 B | 75,608 B | 127,072 B | 8,340 B | 0 |

인메모리 요청의 한 번은 `cwist_app_dispatch_memory()`가 호출자에게 반환하는
응답 버퍼입니다. lwIP/TLS는 warm-up된 연결의 steady-state를 측정합니다. cJSON
핸들러처럼 애플리케이션이 할당하는 비용은 별도로 더해야 합니다.

### 요청 arena

요청과 응답 객체는 두 개의 arena를 사용합니다. 비호스트 빌드는 첫 사용 후
arena 구조체와 기본 크기 버퍼를 재활용하므로 반복 요청의 힙 단편화를 막습니다.
기본값은 8 KB generation 두 개이며, 최대 버퍼 4개와 구조체 4개를 캐시합니다.

작은 부품에서는 공통 경로 용량을 빌드 시 조정할 수 있습니다.

```sh
make baremetal BAREMETAL_EXTRA_CFLAGS=-DCWIST_ARENA_DEFAULT_GENERATION_BYTES=4096
```

generation을 초과하는 할당은 arena에서 `NULL`을 반환하고 호출자의 힙 fallback을
사용합니다. arena를 줄여도 요청 제한은 바뀌지 않으며, 핸들러나 반환 응답의
할당까지 없어지는 것은 아닙니다.

### 연결과 응답

32-bit ARM에서 plain 연결의 어댑터 제어 블록은 약 60바이트이며 다음 메모리가
추가됩니다.

- 최대 `max_header_bytes + max_body_bytes + 1`까지 증가하는 수신 버퍼
- 가장 큰 직렬화 응답까지 증가한 후 keep-alive 요청에 재사용하는 송신 버퍼
- lwIP의 PCB, pbuf, segment, timer pool

송신 버퍼는 평균이 아닌 최대 응답 기준으로 계산해야 합니다. `max_conns`, 본문
제한, 핸들러 출력을 함께 제한하십시오. 현재 파서/직렬화기는 최소 16 KB의 메인
스택 여유를 권장하며, 최종 애플리케이션에서 다시 측정해야 합니다.

TLS는 여기에 mbedTLS 세션, 최대 `MBEDTLS_SSL_IN_CONTENT_LEN + 2048`의 암호문
입력, HTTP 버퍼를 더합니다. 테스트 프로파일의 서버 측 TLS 세션은 어댑터 버퍼
전 약 25 KB입니다. 임의 클라이언트의 전체 크기 record를 받기 위해 입력 record
버퍼는 16 KB를 유지합니다.

### lwIP pool

예제의 초기값입니다. 디버그 빌드에서는 링커 맵과 runtime pool 통계를 함께 보고
조정하십시오.

| 옵션 | 초기값 | 의미 |
|---|---:|---|
| `MEM_SIZE` | 16 KB | CWIST/newlib 힙과 별개인 lwIP 전용 힙 |
| `MEMP_NUM_TCP_PCB` | 6 | 리스너 + 연결 4개 + 여유 |
| `MEMP_NUM_TCP_SEG` | 16 | 대기 중인 송신 segment |
| `PBUF_POOL_SIZE` | 12 | RX/TX와 네트워크 드라이버의 패킷 버퍼 |
| `TCP_WND` | `4 * TCP_MSS` | 수신 윈도와 입력 상한 |
| `TCP_SND_BUF` | `2 * TCP_MSS` | 송신 back-pressure와 스트리밍 구간 |

제공하는 raw-only 예제는 `NO_SYS=1`, `LWIP_NETCONN=0`, `LWIP_SOCKET=0`이
필요합니다. `MEMP_NUM_TCP_PCB`는 리스너와 모든 수락 연결을 포함해야 합니다.
DHCP, DNS 또는 netif를 추가하면 pool도 늘리십시오.

## HTTPS

어댑터를 `CWIST_LWIP_TLS`로 빌드하고 mbedTLS 3.6을 링크한 다음 `cfg.tls`에
`cwist_lwip_tls_create()`의 컨텍스트를 지정합니다. 현재 프로파일은 TLS 1.2
서버이며 ECDHE-ECDSA P-256, AES-128-GCM-SHA256을 사용합니다. 인증서/키 파싱과
DRBG 시딩은 실패 시 닫히며, TLS 설정이 plaintext로 대체되지 않습니다.

MCU의 CRYP/HASH/PKA 가속은 CWIST 훅이 아니라 펌웨어의 `MBEDTLS_*_ALT` 설정
영역입니다. TLS 1.3, 세션 티켓, 재접속은 포함하지 않습니다. 제품에 적용하기 전에
record 버퍼 크기를 검토하십시오.

## 제한사항

- `cwist_app_listen()`, 소켓, worker pool, RTOS/netconn, W5500 어댑터는 없습니다.
- HTTP/2, HTTP/3, WebSocket 업그레이드, deferred `cwist_async`, 파일 스트리밍은
  bare-metal 어댑터에서 지원하지 않습니다.
- 요청 chunk 재조립, DB/SQLite, NATS/Redis, OTel 런타임은 아카이브에 없습니다.
- `qemu-system-arm` 보드 모델이나 실제 보드 통과를 주장하지 않습니다.
  `example/stm32/README.md`의 하드웨어 smoke와 네트워크 확인이 필요합니다.
