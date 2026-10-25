# 릴리스 사전 빌드 에셋

공개된 모든 GitHub 릴리스에는 소스 tarball `cwist-<version>.tar.gz` 옆에, CI가
빌드하고 검증하는 사전 빌드 타깃이 함께 올라갑니다.
`.github/workflows/release-assets.yml`이 릴리스가 공개될 때 릴리스 태그에서 이를
빌드하고, 각각 스모크 테스트를 거친 뒤 체크섬 파일과 함께 첨부합니다.
`<version>`은 태그에서 앞의 `v`를 뗀 값입니다(`v4`는 `4`, `v4.1`은 `4.1`).

| 에셋 | 내용 | 빌드 환경 |
| --- | --- | --- |
| `cwist-<version>-fat-linux-x86_64.tar.gz` | `make fat-dist` 번들, BoringSSL | ubuntu-22.04, gcc |
| `cwist-<version>-fat-linux-x86_64-mbedtls.tar.gz` | `make CWIST_TLS=mbedtls CWIST_HTTP3=0 fat-dist` 번들 | ubuntu-22.04, gcc |
| `cwist-<version>-fat-linux-aarch64.tar.gz` | `make CROSS_COMPILE=aarch64-linux-gnu- fat-dist` 번들 | ubuntu-22.04, 크로스 gcc |
| `cwist-<version>-fat-macos-arm64.tar.gz` | `make fat-dist` 번들(libtool 병합) | macos-15, Apple clang |
| `cwist-<version>-amalgamation-linux.tar.gz` | `make amalgamation`: `cwist.c`와 `include/` | ubuntu-22.04 |
| `cwist-<version>-amalgamation-linux-tiny.tar.gz` | `make CWIST_TINY=1 amalgamation` | ubuntu-22.04 |
| `cwist-wasm-<version>.tgz` | `make wasm-dist`: cwist-wasm npm 패키지 | ubuntu-latest, Emscripten 5.0.0 |
| `cwist-<version>-assets-SHA256SUMS` | 위 모든 에셋의 sha256 | |

소스 tarball은 이 워크플로가 빌드하지 않습니다. 메인테이너가 올리며, Homebrew
formula가 그 체크섬을 고정합니다.

## fat 번들 사용하기

번들을 풀면 `cwist-<version>-fat-<target>/` 아래에 `lib/libcwist.a`(CWIST와
모든 vendored 아카이브)와 `include/`가 있습니다. 시스템 라이브러리만 외부에
남습니다.

```sh
cc -O2 -std=gnu17 -Icwist-4-fat-linux-x86_64/include -o app main.c \
  cwist-4-fat-linux-x86_64/lib/libcwist.a \
  $(pkg-config --libs libcurl libnghttp2 libbrotlienc libbrotlicommon libbrotlidec libzstd) \
  -pthread -ldl -lm -lstdc++ -lz
```

macOS에서는 `-ldl -lstdc++` 대신 `-lc++`를 씁니다. 첨부 전에
`scripts/ci/bundle_smoke.sh`가 모든 번들에 대해 실행하는 링크 명령이 이것입니다
(amalgamation도 `cwist.o`를 아카이브 앞에 링크해 같은 스크립트로 검증합니다).

- **CPU 기준.** 번들은 `CWIST_PORTABLE=1`로 빌드됩니다. `-march=native`가 없으므로
  대상 아키텍처의 어떤 CPU에서도 실행됩니다. 호스트에 맞춘 코드가 필요하면
  소스에서 빌드하세요.
- **glibc.** Linux 번들은 Ubuntu 22.04(glibc 2.35)에서 빌드되어 그보다 새로운
  glibc 심볼을 참조하지 않으므로, 해당 릴리스 이후 환경에서 링크됩니다.
- **armhf 번들 없음.** libttak이 아직 32비트 ARM에서 빌드되지 않습니다
  (`src/accel/accel_cpu.c`가 `__int128`을 요구).

## amalgamation 사용하기

amalgamation에는 CWIST 코드만 들어 있습니다. vendored 라이브러리는 같은 구성의
fat 번들에서 링크합니다(예: 기본 amalgamation에는 `fat-linux-x86_64`). 컴파일
명령은 `-I` 하나와 파일 하나입니다.

```sh
gcc -O2 -std=gnu17 -Ibundle/include -pthread -c bundle/cwist.c -o cwist.o
```

빌드 시점의 Linux 플랫폼 정의를 담고 있어 이름에 `linux`가 붙습니다.

## 다시 실행하기

워크플로는 기존 태그를 지정해 수동으로 실행할 수 있습니다(Actions, "Release
assets", Run workflow). 예를 들어 워크플로가 생기기 전에 공개된 릴리스에 에셋을
첨부할 때 씁니다. 에셋은 모든 빌드와 스모크 테스트가 통과한 뒤에만 올라가며,
다시 실행하면 이 워크플로가 관리하는 에셋을 교체합니다.
