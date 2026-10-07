# CWIST 문서 지도 (한국어)

CWIST를 배우고, 사용하고, 확장하는 데 필요한 모든 문서를 권장 읽기 순서대로 정리했습니다.

> 이 디렉터리는 `docs/`의 한국어 번역입니다. 원문과 내용이 다르면 영어 원문이 기준입니다.
> 영어 원문: [docs/README.md](../README.md)

## 1. 시작하기

* [README](../../README.md) - 설치, hello world, 기능 소개, 벤치마크.
* [튜토리얼 시리즈](../../tutorials/README.md) - 30개의 실습 모듈. 각 디렉터리에는 실행 가능한
  `main.c`, `CMakeLists.txt`, 단계별 가이드가 들어 있습니다.
  [01-hello-world](../../tutorials/01-hello-world/README.md)에서 시작해 번호 순서대로
  따라가세요. 뒤쪽 모듈은 앞 모듈의 개념을 전제로 합니다.

## 2. 작업 중심 가이드

여러 서브시스템을 조합하는 긴 안내서입니다.

* [Wasmtime 어플라이언스 배포](deployment/wasmtime-appliance.md) - WASI 예제의 빌드, 격리된
  상태, 재시작 검증, 백업, 롤백.
* [CWIST로 CRUD 블로그 만들기](tutorials/blog-crud.md) - 라우팅 + SQLite + JSON.
* [NATS 연동](tutorials/nats-integration.md) - 핸들러에서 메시징하기.
* [WebTransport 서버](tutorials/webtransport-server.md) - 실험적인 HTTP/3
  datagram/stream API.
* [OpenAPI와 Swagger UI](tutorials/openapi-swagger.md) - 라우트에 주석을 달고
  `openapi.json`을 생성한 뒤 대화형 문서 페이지를 제공합니다.
* [CWIST 튜토리얼 (한 페이지)](../tutorial/korean/cwist_tutorial.md) - 한 파일로 압축한
  한국어 튜토리얼.

## 3. API 레퍼런스

* [API 모듈 색인](API.md) - `api/` 아래의 모듈별 문서.
* [간단 레퍼런스](api-quickref.md) - 공개 함수를 한 파일에 나열한 목록.
* [REFERENCE.md](REFERENCE.md) - 구조와 동작에 대한 레퍼런스.
* [Doxygen HTML](https://c4punks.github.io/CWIST/) - 주석이 달린 공개 헤더
  (`include/cwist/`)에서 생성한 문서.

## 4. 실행 가능한 예제

[`example/`](../../example/) 아래의 독립 실행형 데모입니다.

| 예제 | 보여 주는 내용 |
|---------|--------------|
| [simple-server](../../example/simple-server) | 최소 HTTP 서버 |
| [http](../../example/http) | 단계별 HTTP 기능 |
| [db](../../example/db) / [db-crypt](../../example/db-crypt) | SQLite 연동, 투명한 컬럼 암호화 |
| [jwt](../../example/jwt) | 토큰 발급과 검증 |
| [websocket / othello-web](../../example/othello-web) | WebSocket 게임 서버 |
| [rps-showcase](../../example/rps-showcase) | 정적 캐싱 + 처리량 데모 |
| [json-builder](../../example/json-builder), [html](../../example/html), [template](../../example/template) | 렌더링 도우미 |
| [micro](../../example/micro), [mem](../../example/mem), [siphash](../../example/siphash), [sstring](../../example/sstring) | 핵심 유틸리티 |
| [cde-json-viewer](../../example/cde-json-viewer) | JSON 뷰어 앱 |
| [webtransport](../../example/webtransport) | 실험적 WebTransport |

## 5. 벤치마크와 측정 방법

* [웹 서버 벤치마크 측정 방법](webserver-benchmark.md) - README의 CI 수치를 만드는 방법.
* [benchmark-trends.svg](../benchmark-trends.svg) / [webserver-benchmark-trends.svg](../webserver-benchmark-trends.svg) -
  CI 기록 추이.

## 6. 참고 자료

* [Mux 라우터 알고리즘 노트](references/mux_algorithm_references.md)
* [릴리스 코드네임 순서 규칙](../versioning.md) - 순차 전파 규칙 (원문이 한국어입니다).
* [ROADMAP.md](../../ROADMAP.md) - 기능 현황과 마일스톤 계획.

## 번역된 문서 목록

| 원문 | 번역 |
|---|---|
| [GC.md](../GC.md) | [GC.md](GC.md) |
| [async-gc-ownership.md](../async-gc-ownership.md) | [async-gc-ownership.md](async-gc-ownership.md) |
| [c1m-file-limits.md](../c1m-file-limits.md) | [c1m-file-limits.md](c1m-file-limits.md) |
| [classic-pool-starvation.md](../classic-pool-starvation.md) | [classic-pool-starvation.md](classic-pool-starvation.md) |
| [cooperative-queuing.md](../cooperative-queuing.md) | [cooperative-queuing.md](cooperative-queuing.md) |
| [durable-queue-gate.md](../durable-queue-gate.md) | [durable-queue-gate.md](durable-queue-gate.md) |
| [fixed-cache-status.md](../fixed-cache-status.md) | [fixed-cache-status.md](fixed-cache-status.md) |
| [http-reactor-fairness.md](../http-reactor-fairness.md) | [http-reactor-fairness.md](http-reactor-fairness.md) |
| [reactor-wakeup-regression.md](../reactor-wakeup-regression.md) | [reactor-wakeup-regression.md](reactor-wakeup-regression.md) |
| [performance/wrk-dual-histogram.md](../performance/wrk-dual-histogram.md) | [performance/wrk-dual-histogram.md](performance/wrk-dual-histogram.md) |
| [decisions/0001-safe-public-fixed-response-cache.md](../decisions/0001-safe-public-fixed-response-cache.md) | [decisions/0001-safe-public-fixed-response-cache.md](decisions/0001-safe-public-fixed-response-cache.md) |
| [webserver-benchmark.md](../webserver-benchmark.md) | [webserver-benchmark.md](webserver-benchmark.md) |
| `api/*.md` | [api/](api/) |
| `tutorials/*.md` | [tutorials/](tutorials/) |
