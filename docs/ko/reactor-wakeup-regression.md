# io_uring 게시 작업 깨우기 회귀 (#25)

## 범위와 원인

`7a67f87d`에서 들어온, `dev`의 `6afc4a3a506e6740d93ebb2630de917fafd7dea9`에 있던 깨우기 회귀를
수정합니다. 이는 [#25](https://github.com/c4punks/CWIST/issues/25)에 대한 부분적인 기여이며,
원래의 HTTP 동시성 64/256/512 P99.999 문제가 완전히 해결되었다는 근거가 아닙니다.

`IORING_REGISTER_EVENTFD`는 **링에서 eventfd로 완료 알림**을 보냅니다. 그 eventfd에 쓴다고
해서 CQE가 큐에 들어가거나 완료를 기다리는 링이 깨어나지는 않습니다. 이전 코드는 이를 반대
방향으로 사용했고, 깨우기 fd의 입력 poll을 없앴습니다. 그래서 관련 없는 소켓 활동이 없으면 게시된
작업이 실행 루프의 100 ms 종료 확인 타임아웃까지 기다릴 수 있었습니다. 등록된 eventfd 알림이
`user_data == 0`인 CQE를 만들어 내는 것도 아닙니다.

수정은 io_uring에서 단발성 입력 poll과 그 기존 콜백/재무장 경로를 되살립니다. 빈 상태에서 비어
있지 않은 상태로 바뀔 때의 post 스택 깨우기 병합은 계속 켜져 있습니다. 공개 API나 연결 레이아웃
변경은 없습니다.

참고 자료:

- [io_uring_register(2), IORING_REGISTER_EVENTFD](https://man7.org/linux/man-pages/man2/io_uring_register.2.html)
- [io_uring_register_eventfd(3)](https://man7.org/linux/man-pages/man3/io_uring_register_eventfd.3.html)

## 회귀 테스트

`test_reactor_wake`는 테스트 전용 libc 할당자와 종료 플래그로 프로덕션 리액터를 컴파일합니다.
커널 polling, eventfd, 게시, 디스패치는 대체하지 않습니다. 체크아웃된 libttak 헤더가 필요하지만
HTTP/TLS 스택이나 libttak 할당자 런타임은 링크하지 않습니다.

io_uring에서는 외부 스레드가 노드 128개짜리 묶음을 네 번 게시하고, 묶음마다 실제 커널 완료를
요구하며, 묶음 사이에 프로덕션 디스패치를 실행해 재무장을 실험합니다. 타임아웃은 완료를 대신할
수 없고, 제출 수가 양수인 것만으로도 충분하지 않습니다. 유휴 링 검사는 완료에서 eventfd로 가는
되먹임을 거부합니다. 지원하는 모든 백엔드에서 외부 스레드 post 64개를 더 보내 리액터 소유자에서
정확히 한 번, FIFO 순서로 전달되는지 확인합니다. 30초 알람은 멈춤 감시일 뿐 성능 수용 관문이
아닙니다.

```sh
git submodule update --init lib/libttak
CWIST_TEST_REQUIRE_IO_URING=1 make WERROR=1 test_reactor_wake
CWIST_REACTOR_BACKEND=epoll make WERROR=1 test_reactor_wake
CWIST_TEST_REQUIRE_IO_URING=1 make WERROR=1 SANITIZE=address,undefined test_reactor_wake
CWIST_REACTOR_BACKEND=epoll make WERROR=1 SANITIZE=address,undefined test_reactor_wake
# macOS, native kqueue:
make CC=clang WERROR=1 SANITIZE=address,undefined test_reactor_wake
```

Linux 명령은 `io_uring_setup`을 허용하는 호스트에서 실행하세요. 컨테이너의 seccomp 정책이 이를
금지할 수 있습니다. 필수 백엔드 변수 덕분에 조용히 epoll을 테스트하는 대신 명시적으로 실패합니다.
의도적으로 epoll을 테스트할 때는 그 변수를 남겨 두지 마세요. 새니타이저 CI 워크플로는 기존 전체
스위트에 더해 Linux 백엔드 두 가지를 명시적으로 테스트합니다.

## 구성 요소 측정 (2026-09-12)

환경: Linux `6.8.0-117-generic`, GCC `13.3.0`, ARM64 Mac 위의 x86_64 Colima/QEMU 게스트, vCPU
6개와 RAM 12 GiB로 설정. 공유되는 에뮬레이션 VM이며, 프로덕션 처리량 벤치마크가 아닙니다. 각
바이너리는 같은 테스트와 `-std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread`로 빌드했고 리액터
소스만 달랐습니다. `--bench`는 의도적으로 실패하는 커널 회귀 관문을 건너뛰고, 게시 직전부터
콜백 실행까지의 `CLOCK_MONOTONIC` 시간을 기록합니다. 명시적 워밍업 없음, 소켓 없음, 생산자
하나와 리액터 하나, 미처리 post는 한 번에 하나씩. 기존 100 ms 실행 루프 타임아웃은 그대로였습니다.

짝지은 실행 세 번, 바이너리당 샘플 64개, 순서는 base/fixed, fixed/base, base/fixed:

- 기준(base) 실행 중앙값: 103.955, 103.537, 103.490 ms.
- 수정(fixed) 실행 중앙값: 0.033433, 0.036471, 0.038093 ms.
- 바이너리당 샘플 192개 전체에서 base 중앙값/최댓값: 103.787046 / 109.251727 ms.
- 바이너리당 샘플 192개 전체에서 fixed 중앙값/최댓값: 0.034446 / 0.273538 ms.

회귀 관문은 기준에 대해 `posted wake did not generate a completion: Timer expired`로 실패하고,
수정 후에는 통과합니다. 대상 ASan/UBSan 검사는 Linux io_uring, 강제 epoll, 네이티브 macOS
kqueue에서 통과합니다. 이것은 구성 요소 검사이지 할당자 통합, HTTP 처리량, P99.999 측정이
아닙니다. 원래 이슈의 HTTP 작업과 프로덕션 하드웨어는 여전히 별도의 종단 간 검증이 필요합니다.
