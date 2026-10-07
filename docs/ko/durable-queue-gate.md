# 영속 큐 필수 게이트

`tests/test_durable_queue.c`는 Redis와 NATS JetStream에서 enqueue/claim/ack, nack 재전달,
가시성 타임아웃, 데드레터 동작을 다룹니다. 로컬 `make test` 실행에서는 서버가 없어도 선택
사항으로 남습니다.

`CWIST_TEST_REQUIRE_DURABLE_QUEUE=1`이면 서버 부재, JetStream 사용 불가, 큐 팩토리 에러, 시나리오
실패가 모두 테스트 실패가 됩니다. 성공하려면 두 백엔드가 모든 시나리오를 실행해야 합니다.
설정하지 않거나, 비어 있거나, `0`이면 기존의 선택 동작을 유지하고, 그 밖의 비어 있지 않은 값은
엄격 모드를 켭니다. Redis 팩토리 에러는 선택 모드에서도 계속 실패로 처리됩니다.

## 로컬 실행

```sh
make test_durable_queue
CWIST_TEST_REQUIRE_DURABLE_QUEUE=1 ./test_durable_queue
```

이 바이너리는 기존 `CWIST_REDIS_HOST`, `CWIST_REDIS_PORT`, `CWIST_NATS_URL` 설정을 따릅니다.
테스트용 인스턴스만 가리키도록 하세요.

## 격리된 통합 게이트

`redis-server`, `nats-server`, Python 3을 설치한 뒤 다음을 실행합니다.

```sh
make test_durable_queue
python3 scripts/ci/durable_queue_gate.py
python3 -m unittest discover -s scripts/ci -p 'test_durable_queue_gate.py'
```

게이트는 루프백 전용 Redis와 NATS 자식 프로세스를 직접 띄웁니다. 서버가 없는 경우를 포함해
사용하지 않는 임의의 포트를 예약하며, 기본적으로 6379나 4222 포트는 절대 쓰지 않습니다. 선택적인
`CWIST_DQ_REDIS_PORT`, `CWIST_DQ_NATS_PORT`, `CWIST_DQ_NOJS_PORT` 덮어쓰기 값은 사용 중이지
않은 포트여야 합니다. 사용 중인 포트는 어떤 테스트 요청보다 먼저 실패합니다.
`CWIST_DQ_REDIS_BIN`과 `CWIST_DQ_NATS_BIN`은 브로커 실행 파일을 고릅니다. 시작 대기 시간은
`CWIST_DQ_READY_SECS`(기본 20, 최대 120)로 제한되며, 테스트 호출마다 60초 타임아웃이 있습니다.

일곱 가지 모드는 다음과 같습니다.

1. 선택 모드, 둘 다 없음: 두 건너뛰기 메시지와 함께 종료 코드 0.
2. 엄격 모드, 둘 다 없음: 두 REQUIRED 진단과 함께 종료 코드 1.
3. 엄격 모드, Redis 동작 중이고 NATS 없음: 종료 코드 1; Redis 테스트는 통과해야 함.
4. 엄격 모드, Redis 동작 중이고 JetStream 비활성: 종료 코드 1; Redis 테스트는 통과해야 함.
5. 엄격 모드, Redis와 JetStream 동작 중: 종료 코드 0; 두 스위트 모두 통과해야 함.
6. 엄격 모드, Redis 없음, JetStream 동작 중: 종료 코드 1; NATS 테스트는 통과해야 함.
7. 명시적 선택 모드(`0`), 둘 다 없음: 두 건너뛰기 메시지와 함께 종료 코드 0.

부정적인 결과는 정확히 종료 코드 1과 특정 진단을 요구하며, 크래시와 타임아웃은 인정하지
않습니다. 임시 데이터와 직접 띄운 자식 프로세스는 성공, 실패, INT, TERM 모두에서 정리됩니다.
로컬 게이트는 `FLUSHALL`, 외부 브로커 종료, 전역 설치, 프로덕션 큐 API 변경을 하지 않습니다.

## CI

`.github/workflows/durable-queue.yml`은 Ubuntu 러너에 체크섬으로 고정한 Redis 8.0.3과 NATS
2.12.3을 설치하고, 실제 테스트 바이너리를 빌드한 뒤, 도우미 테스트와 같은 일곱 모드 게이트를
실제로 실행합니다. 이 작업은 `contents: read` 권한만 요청하며 30분 제한이 있습니다. 이것은 기존
큐 시나리오를 테스트하는 것이며, 브로커 재시작 영속성이나 크래시 복구 의미를 테스트하지는
않습니다.
