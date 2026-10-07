# Wasmtime으로 WASI 0.2 예제 배포하기

이 안내는 [edge-KV 예제](../../../example/wasip2-kv/README.md)를 단일 인스턴스 Wasmtime
어플라이언스로 실행합니다. 버전이 붙은 `app.wasm` 산출물, 별도의 쓰기 가능한 상태 디렉터리, 그리고
그 수명을 소유하는 호스트 프로세스로 구성됩니다. POSIX 호스트(Linux 또는 macOS)에서의 빌드, 시작,
검증, 재시작, 백업, 롤백을 다룹니다. 시스템 서비스는 설치하지 않습니다.

## 범위와 안전

이것은 배포 예제이며 **프로덕션용 데이터베이스 서비스가 아닙니다**. 격리된 개발 호스트나
네트워크에서 버려도 되는 데이터를 쓰세요.

- 게스트는 루프백만이 아니라 **모든 IPv4 인터페이스의 18100 포트**에서 평문 HTTP를 제공합니다.
  예제에는 인증이 없습니다. 시작하기 **전에** 호스트 방화벽이나 네트워크 격리로 신뢰할 수 없는
  네트워크의 접근을 막으세요. 리버스 프록시만으로는 18100으로의 직접 접근이 닫히지 않습니다.
- `-S inherit-network=y`는 호스트 네트워크에 대한 넓은 접근을 허용합니다. localhost 전용이나
  인바운드 전용 허용이 아닙니다. 더 좁은 네트워크 정책은 호스트 방화벽 규칙이나 사용자 정의 임베딩이
  강제해야 합니다.
- 상태 디렉터리당 쓰는 쪽은 정확히 하나만 실행하세요. 각 프로세스는 자기만의 인메모리
  데이터베이스를 불러오고 변경 후 blob 전체를 덮어씁니다. 파일을 공유하는 두 복제본은 서로의 갱신을
  잃을 수 있습니다.
- blob 쓰기는 원자적 교체나 `fsync`가 아니라 `fopen(..., "wb")`와 `fwrite`를 씁니다. 완료된
  POST가 정전에 대한 영속성을 보장하지 않습니다. 영속화 에러가 나면 인메모리 삽입은 적용된 채
  파일이 잘릴 수 있으며, POST를 재시도하면 삽입이 중복될 수 있습니다.
- 파일이 없거나 불러올 수 없으면 시작 시 빈 데이터베이스로 대체될 수 있습니다. `GET /`를 영속
  데이터 무결성의 증거로 쓰지 마세요. 예상한 데이터가 없으면 쓰기를 멈추고 blob/백업을 조사하세요.

Cloudflare Workers는 이 바이너리를 그대로 올릴 수 있는 호스트가 **아닙니다**. 이 바이너리는
소켓을 바인딩하고 WASI 파일 시스템 호출을 쓰는 `wasi:cli/run` 명령입니다. `wasmtime serve`는
다른 인터페이스인 `wasi:http/proxy`를 기대합니다. Workers로 옮기려면 호스트 어댑터와 다른
요청/영속성 경계가 필요하며, blob 직렬화 API만으로는 그것을 제공하지 못합니다.

## 1. 알려진 리비전 빌드하기

Git, make, curl, Python 3, `wasm32-wasip2`를 지원하는 wasi-sdk, 컴포넌트 모델과 `wasi:sockets`를
지원하는 Wasmtime을 설치하세요. 도구는 공식
[wasi-sdk 릴리스](https://github.com/WebAssembly/wasi-sdk/releases)와
[Wasmtime 릴리스](https://github.com/bytecodealliance/wasmtime/releases)에서 받고, 릴리스 산출물을
검증하고, 호스트 아키텍처를 올바르게 고르세요. 실제 배포에는 유지보수되고 보안 패치가 된 Wasmtime
릴리스를 쓰세요. 저장소의 CI 호환성 기준선은 wasi-sdk 25.0 / Wasmtime 25.0.0이며, 아래 명령은
macOS arm64에서 wasi-sdk 25.0 / Wasmtime 48.0.2로도 실행해 보았습니다.

새 체크아웃에서 배포하려는, 검토를 마친 `dev` 커밋을 선택하세요 (릴리스 브랜치에 이 예제가 있다고
장담할 수 없습니다).

```sh
git clone --branch dev https://github.com/c4punks/CWIST.git cwist-appliance-src
cd cwist-appliance-src
git checkout --detach <reviewed-dev-commit>
git submodule update --init --recursive
```

아래 도구 경로를 호스트의 절대 경로로 바꾸세요. 이것과 나머지 셸 조각은 체크아웃 루트에서 같은
셸로 실행합니다.

```sh
export WASI_SDK=/absolute/path/to/wasi-sdk
export WASMTIME=/absolute/path/to/wasmtime
"$WASI_SDK/bin/clang" --version
"$WASMTIME" --version
make -j4 libcwist_wasip2.a
make wasip2-smoke
./example/wasip2-kv/build.sh
```

SDK, 소스 리비전, 컴파일러 플래그를 바꿀 때는 깨끗한 체크아웃을 쓰세요. `build.sh`는 라이브러리
아카이브를 다시 빌드하지 않고 기존 것을 재사용합니다. 게스트는 `-Wl,-z,stack-size=1048576`으로
링크합니다. 직접 링크 명령을 쓸 때도 이 플래그를 유지하세요. 이것은 선형 메모리 안의 게스트 C
스택이며 Wasmtime의 네이티브 스택 설정이 아닙니다. [WASI 레퍼런스](../api/wasi.md)를 보세요.

`make wasip2-smoke`는 18099 포트를 씁니다. 18099와 예제의 18100이 모두 비어 있는지 확인하세요.
예제의 포트는 `app.c`에 `KV_PORT`로 컴파일되어 있어서, 호스트의 `PORT` 환경 변수로는 바이너리를
다시 설정할 수 없습니다.

## 2. 산출물과 상태 분리하기

이 일회용 안내에서는 새 비공개 어플라이언스 디렉터리를 만듭니다. 오래 쓰는 어플라이언스라면 임시
경로 대신 영구 저장소에, root가 아닌 서비스 계정이 소유하는 같은 구조의 디렉터리를 준비하세요.

```sh
umask 077
APPLIANCE=$(mktemp -d "${TMPDIR:-/tmp}/cwist-appliance.XXXXXX")
REV=$(git rev-parse HEAD)
mkdir -p "$APPLIANCE/releases/$REV" "$APPLIANCE/state" "$APPLIANCE/logs"
cp example/wasip2-kv/app.wasm "$APPLIANCE/releases/$REV/app.wasm"
chmod 0444 "$APPLIANCE/releases/$REV/app.wasm"
MODULE="$APPLIANCE/releases/$REV/app.wasm"
printf 'Appliance directory: %s\nRevision: %s\n' "$APPLIANCE" "$REV"
```

게스트에는 `state`만 `kv`라는 이름으로 미리 열어 줍니다. 모듈과 로그는 그 파일 시스템 허가 밖에
있습니다. 호스트는 절대 경로로 모듈을 읽으며, 게스트는 소스나 릴리스 디렉터리에 접근할 필요가
없습니다. 릴리스마다 소스 리비전, 도구 버전, 산출물 다이제스트를 기록하세요.

```sh
python3 - "$MODULE" <<'PY'
import hashlib, pathlib, sys
p = pathlib.Path(sys.argv[1])
print(hashlib.sha256(p.read_bytes()).hexdigest(), p)
PY
```

## 3. 시작하고 검증하기

```sh
start() {
    "$WASMTIME" run -S preview2=y -S tcp=y -S udp=n \
        -S allow-ip-name-lookup=n -S inherit-network=y \
        --dir "$APPLIANCE/state::kv" "$MODULE" \
        >>"$APPLIANCE/logs/server.log" 2>&1 &
    PID=$!
}
stop() {
    kill "$PID"
    wait "$PID" || :
    unset PID
}
start
```

Wasmtime 플래그는 모듈 경로보다 앞에 와야 합니다. `HOST_DIR::GUEST_DIR`은 호스트의
`state/cwist.db`를 게스트에서 `kv/cwist.db`로 보이게 합니다. UDP와 DNS를 꺼도 TCP 허가는 제한되지
않습니다. 여기서는 `serve`가 아니라 `wasmtime run`을 씁니다. PID는 게스트 신호 대상이 아니라 호스트
Wasmtime 프로세스입니다.

시작을 기다린 뒤, 프로세스가 종료되지 않았는지 확인하세요 (예: 다른 서비스가 이미 포트를 쓰는
경우). 실패하면 멈추고 로그를 읽으세요.

```sh
ready=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    if curl --fail --silent --max-time 2 http://127.0.0.1:18100/items; then
        ready=1
        break
    fi
    sleep 1
done
[ "$ready" = 1 ] && kill -0 "$PID"
```

새 상태 디렉터리에서 `/items`는 `[]`를 반환합니다. 마지막 검사가 실패하면 POST로 넘어가지 마세요.
`$APPLIANCE/logs/server.log`를 살펴보고 실패를 고친 뒤 다시 시작하세요. 준비 응답은 현재 조회가
가능하다는 것만 증명하며, 백업의 유효성이나 크래시 영속성을 증명하지 않습니다.

```sh
curl --fail --show-error --silent --max-time 5 \
    -H 'Content-Type: application/json' \
    -d '{"name":"edge","qty":7}' http://127.0.0.1:18100/items
curl --fail --show-error --silent --max-time 5 http://127.0.0.1:18100/items
```

새 데이터베이스에서 POST는 `{"ok":true,"id":1}`을 반환합니다. 목록에는
`{"id":"1","name":"edge","qty":"7"}`이 들어 있습니다. 이 예제의 조회 콜백은 SQLite 값을 JSON
문자열로 인코딩합니다. 상태와 반환된 내용을 확인하고, 실패한 POST를 무작정 재시도하지 마세요.

## 4. 재시작, 백업, 복원

프로세스를 멈추기 전에 들어오는 쓰기를 멈추세요. Wasmtime을 멈춰도 게스트의 정상 종료가 호출되거나
진행 중인 변경이 기록되지 않습니다. 예제는 POST 중에 blob을 쓰므로, 요청이 끝난 뒤에만 멈추세요.

```sh
stop
test -s "$APPLIANCE/state/cwist.db"
BACKUP="$APPLIANCE/cwist.db.backup"
cp "$APPLIANCE/state/cwist.db" "$BACKUP"
start
```

위의 준비 확인과 `GET /items`를 반복하세요. 같은 행이 반환되어야 합니다. 소중한 예제 데이터에
대해 `example/wasip2-kv/smoke.sh`를 실행하지 마세요. 그 스크립트는 깨끗한 상태를 위해 **자신의
`kv/cwist.db`를 삭제**하고, 기존 `app.wasm`도 재사용합니다. 별도의 어플라이언스 상태는 그 테스트
디렉터리가 아닙니다.

활성 상태나 그 백업을 덮어쓰지 않고 복원을 연습하려면, 프로세스를 멈추고 새 어플라이언스
디렉터리로 복원하세요.

```sh
stop
RESTORE=$(mktemp -d "${TMPDIR:-/tmp}/cwist-restore.XXXXXX")
mkdir -p "$RESTORE/state" "$RESTORE/logs"
cp "$BACKUP" "$RESTORE/state/cwist.db"
APPLIANCE="$RESTORE"
start
```

`MODULE`은 여전히 원래 릴리스 산출물을 가리킵니다. 준비 확인과 `GET /items`를 반복하고, 반환된
행을 예상 데이터와 비교한 뒤, 끝나면 `stop`을 호출하세요. 백업과 원래 상태는 그대로입니다.

업그레이드할 때는 먼저 쓰기와 이전 프로세스를 멈추고, 오프라인 백업을 보존하고, 따로 빌드하고
검증한 `MODULE`을 선택하세요. 의도한 상태에 대해 프로세스 하나를 시작하고 기록을 검증하세요. 모듈
롤백은 스키마가 호환될 때만 하고, 그렇지 않으면 맞는 백업을 새 상태 디렉터리로 복원하고 그 백업
이후의 쓰기를 잃는 것을 명시적으로 받아들이세요. 같은 blob에 대해 이전 버전과 새 버전을 동시에
시작하면 절대 안 됩니다.

## 프로덕션에 맞게 고치기 전에

인증, TLS 종단, 요청 한도, 보호된 인그레스, 최소 권한 네트워크 정책, 자원 한도, 모니터링, 호스트
프로세스 감독을 갖추세요. 데이터베이스 영속성 어댑터에도 원자적 쓰기, 에러 전파, 실패 시 닫히는
로드 정책, 정의된 크래시 복구 계약이 필요합니다. 백업 보존과 스키마 마이그레이션은 호스트/애플리케이션의
몫입니다. 현재 예제는 그런 통제를 구현하지 않습니다.

런타임 옵션의 의미는 공식 [Wasmtime CLI 문서](https://docs.wasmtime.dev/cli-options.html)와 설치된
바이너리의 `wasmtime run --help` / `wasmtime run -S help`를 보세요.
