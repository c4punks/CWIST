# 웹 서버 벤치마크 측정 방법

이 문서는 `benchmarks/webserver.json`에 기록되고 `docs/webserver-benchmark-trends.svg`로
렌더링되는 CWIST vs Axum vs Gin vs Spring Boot 비교가 정확히 어떻게 만들어지는지 설명합니다.
그래야 공개된 모든 수치를 재현하고 감사할 수 있습니다.

스위트는 GitHub Actions(`.github/workflows/bsd-kqueue-benchmarks.yml`의 `web-server-benchmark`
작업)에서 `ubuntu-latest`로 실행됩니다. 네 서버 모두 워크플로가 인라인으로 생성하는 최소한의
`GET / -> "Hello, World!"` 애플리케이션이며, 여기 문서화된 것 외에 프레임워크별 튜닝은 적용하지
않습니다.

## 부하 프로필

| 단계 | 명령 | 목적 |
|---|---|---|
| 워밍업 | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | 버림. JIT 계층형 런타임(JVM)이 정상 상태에 도달하게 함. 네 서버에 똑같이 적용. |
| 측정 | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | 기록: `Requests/sec`, 평균 `Latency`. |

**CPU 예산:** 모든 서버와 부하 생성기는 러너의 CPU 전체를 봅니다. 고정(pinning)도, 런타임별
스레드 상한도 없습니다. 각 런타임은 자체 기본/자동 워커 크기를 씁니다 (CWIST
`CWIST_WORKERS=auto`, Tokio `available_parallelism`, Go `GOMAXPROCS=default`, Netty
`ioWorkerCount=nproc`). 이렇게 해야 비교가 공정합니다. 어떤 프레임워크도 다른 프레임워크가 받지
못하는 수동 튜닝의 이점을 얻지 않습니다.

- 서버 시작 대기는 고정 sleep이 아니라 준비 확인 루프(`curl` poll, JVM은 최대 90초)입니다.
- **최대 RSS**는 측정 실행 직후 `ps -o rss=`로 샘플링합니다.
- **컨텍스트 스위치**(`ps`의 `nvcsw + nivcsw`)는 측정 구간에서만 셉니다. 카운터 기준값은 워밍업
  *후에* 잡습니다.

### 컨텍스트 스위치 열 해석하기

CWIST classic(`CWIST_C1M_MODE=0`)은 행렬에서 유일한 블로킹 연결당 스레드 서버입니다. 다른 모든
행은 연결을 작은 스레드 풀에 다중화합니다. 그 차이는 거의 전부 이 열에 나타나며, 낭비된 작업이
아니라 동시성 모델의 성질입니다.

- Classic은 요청당 자발적 스위치를 한 번 냅니다. `recv`에서 블로킹된 워커가 자기 연결의 요청이
  도착하면 깨어납니다. 그것이 블로킹 모델의 하한이며, 처리 경로의 어떤 것도 두 번째 스위치를
  더하지 않습니다 (응답 전송은 `MSG_DONTWAIT`를 쓰고, 클라이언트가 정말 느릴 때만 인라인 poll로
  대체합니다).
- CWIST C1M과 비동기 런타임은 깨어날 때마다 많은 요청을 묶어 처리하므로 요청당 스위치 수가 훨씬
  적습니다.

이는 CPU 할당량을 제한한 상태로 작업을 다시 실행하고 스레드별 자발/비자발 카운터를 나눠서 원인을
확인했습니다. classic 스위치는 거의 모두 자발적인 recv 깨우기(모델의 하한)이며, 풀은 생성 실패
없이 연결당 워커 하나까지 늘어납니다. 낮은 컨텍스트 스위치 운영이 바로 기본 프로필인 C1M의
목적입니다. classic은 그 대신 낮은 동시성에서의 지연 특성(튜닝된 `wrk -t4 -c100` 실행)을
택합니다.

### 튜닝된 저지연 실행

CWIST와 Spring Boot는 각각 별도의 전용 프로세스에서 `wrk -t4 -c100 -d10s`(위 주 프로필
`-t12 -c400`보다 낮은 동시성)로 한 번 더 측정합니다. Spring Boot의 경우 주 실행에 쓴 것과 같은
학습된 AOT 캐시를 재생하는 새 부팅이므로, CWIST가 내지 않는 두 번째 콜드 스타트 비용을 내지
않습니다. 이는 CWIST가 공개하는 저지연 프로필이며, Spring Boot에도 똑같은 처리를 해야 "튜닝된"
수치를 비교할 수 있습니다. 그렇지 않으면 CWIST의 최선의 경우를 Spring이 측정된 적 없는 조건의
수치 옆에 보여 주게 됩니다. Axum과 Gin은 아직 이 두 번째 측정에 포함되지 않습니다.

## CWIST

- 체크아웃한 커밋으로 빌드합니다: `make` 후 `gcc -O3`로 `libcwist.a`에 링크한 벤치 서버.
- 포트 `9091`에서 대기합니다.

## Axum

- `axum = "0.7"`, `tokio = "1"`(`full` 기능), `cargo build --release`.
- 포트 `9092`에서 대기합니다.

## Gin (Go)

- `actions/setup-go`가 준비한 Go 도구(Go **1.22**) 위의 `github.com/gin-gonic/gin` **v1.10.0**,
  `gin.ReleaseMode`로 `go build`, 그 아래는 기본 `net/http` 서버.
- 포트 `9094`에서 대기합니다.

## Spring Boot (JVM 공정성 설정)

JVM은 한 번 재고 끝낼 수 있는 런타임이 아닙니다. 계층형 JIT 컴파일, 힙 크기 조정, 클래스 로딩이
짧은 실행을 지배합니다. 그래서 스위트는 다음 설정을 고정하고 *기록*합니다.

벤치마크하는 스택은 스레드당 요청인 Tomcat 서블릿 컨테이너 위의 Spring MVC가 아니라, Spring의
리액티브 이벤트 루프 서버인 **Reactor Netty 위의 Spring WebFlux**입니다. Netty의 이벤트 루프가
비동기 CWIST(io_uring/epoll)와 Axum(tokio) 서버에 대한 적절한 Java 비교 대상입니다. 이벤트 루프
위에 **가상 스레드를 켜서**(`spring.threads.virtual.enabled=true`, Project Loom) Netty 이벤트
루프 밖으로 보내는 작업이 제한된 플랫폼 스레드 풀 대신 Loom 가상 스레드에서 실행되게 합니다.

| 설정 | 값 |
|---|---|
| Java | Temurin **21** (`actions/setup-java`) |
| Spring Boot | **3.2.3** (`spring-boot-starter-webflux`) |
| 서버 | **Reactor Netty** (이벤트 루프, 논블로킹 I/O) |
| 가상 스레드 | **켜짐** - `spring.threads.virtual.enabled=true` (Project Loom) |
| JVM 옵션 | `-Xms512m -Xmx512m` (고정 힙, 측정 중 크기 조정 잡음 없음) |
| AOT 캐시 | **CDS** - `-XX:ArchiveClassesAtExit=app.jsa`로 학습 실행 (SIGTERM으로 정상 종료), 측정 실행은 `-XX:SharedArchiveFile=app.jsa`로 재생 |
| 워밍업 | 10초 `wrk` 실행, 버림 (위 참고) |
| 포트 | `9093` |

전체 Spring 실행 명령은 다음과 같습니다.

```
java -Xms512m -Xmx512m -XX:SharedArchiveFile=app.jsa -jar spring-bench-0.0.1-SNAPSHOT.jar
```

## 기록하는 메타데이터

`benchmarks/webserver.json`에 추가되는 모든 측정에는 수치 옆에 런타임 정보가 함께 들어가므로,
결과가 고립된 req/s 수치로 남지 않습니다.

```json
{
  "wrk_profile": "wrk -t12 -c400 -d10s (after 10s warmup, warmup discarded)",
  "go_env": {
    "go_version": "go version go1.22.x linux/amd64",
    "framework": "Gin v1.10.0 (gin-gonic/gin, release mode)"
  },
  "spring_env": {
    "java_version": "openjdk version \"21.x\" ... (Temurin)",
    "spring_boot_version": "3.2.3",
    "stack": "Spring WebFlux + Reactor Netty (event loop, virtual threads enabled)",
    "jvm_opts": "-Xms512m -Xmx512m -XX:SharedArchiveFile=... (CDS AOT cache)",
    "virtual_threads": true,
    "aot_cache": "CDS (-XX:ArchiveClassesAtExit training run + -XX:SharedArchiveFile replay)"
  }
}
```

`scripts/ci/benchmark.py render`는 이 환경 블록을 SVG 하단과 README 벤치마크 요약에 출력합니다.

## 지연 분포 차트

`docs/webserver-latency-distribution.svg`(README에서 막대 차트 추이 SVG 바로 아래에 링크됨)는 각
서버의 지연 분포를 밀도 곡선으로 그립니다. 그래서 꼬리의 P99.999 수치만이 아니라 *모양*을 한눈에
볼 수 있습니다. 위 요약 표와 같은 다섯 서버(`cwist`, `cwist_c1m`, `axum`, `gin`, `spring`)를
비교하며, `_tuned` 항목(다른, 더 낮은 동시성의 부하 프로필)과 옵트인 실험 A/B
(`cwist_c1m_arena1`, `cwist_sharded`)는 제외합니다. 그래서 차트는 항상 똑같은
`wrk -t12 -c400 -d10s` 프로필로 만든 실행만 비교합니다.

이것은 **원시 데이터가 아니라 재구성한 것**입니다. wrk는 백분위수(min/p50/p75/p90/p99/p99.9/
p99.99/p99.999/max, 이제 모두 워크플로의 `parse_wrk()`가 수집함.
`.github/workflows/bsd-kqueue-benchmarks.yml` 참고)만 보고하며, 그 아래의 요청별 샘플은 보고하지
않습니다. `scripts/ci/benchmark.py`(`_inverse_cdf_samples`)는 알려진 백분위수 지점 사이에서
역누적분포함수를 선형 보간해 대표 샘플 집합을 합성하고, 그 위에서 표준 가우시안 KDE
(`_gaussian_kde`, 대역폭은 Silverman의 경험 법칙)를 실행합니다. x축은 `log1p(ms)`를 써서, 긴
꼬리(Gin, Spring Boot)가 더 촘촘한 CWIST/Axum 곡선을 왼쪽 끝의 읽을 수 없는 뾰족한 모양으로
눌러 버리지 않게 합니다. 곡선 모양은 정확한 값이 아니라 대표적인 것으로 보세요. 백분위수 데이터가
더 듬성듬성한 서버(새로운 p75/p999/p9999/min/max 필드가 없는 오래된 기록 행)도 렌더링되지만,
보간할 기준점이 더 적을 뿐입니다.

## 알려진 한계

- GitHub 호스팅 러너는 공유되고 잡음이 많은 **vCPU 4개** 머신입니다. 절대 수치는 실험실 수준의
  측정이 아니라 추이 데이터로 보세요. 각 항목은 `runner_hw`(vCPU 수와 CPU 모델)를 정확히
  기록하므로, 서로 다른 머신의 항목을 직접 비교하는 일이 없습니다. 코어를 모두 쓰는 로컬 머신은
  같은 프로필을 CI 러너 속도의 몇 배로 처리합니다. `runner_hw`가 다른 항목 사이의 하락은 코드가
  아니라 머신을 반영합니다.
- JVM의 RSS에는 설계상 예약된 힙(`-Xms512m`)이 포함됩니다. 이는 런타임 모델의 실제 비용이며
  그대로 보고합니다.
- 이 작업은 평문 라우팅 처리량만 측정합니다. TLS, JSON 직렬화, 데이터베이스 접근은 없습니다.
