# Deploy the WASI 0.2 example with Wasmtime

This walkthrough runs the [edge-KV example](../../example/wasip2-kv/README.md)
as a single-instance Wasmtime appliance: a versioned `app.wasm` artifact,
a separate writable state directory, and a host process that owns its lifetime.
It covers build, startup, verification, restart, backup, and rollback on a
POSIX host (Linux or macOS). It does not install a system service.

## Scope and safety

This is a deployment example, **not a production-ready database service**.
Use disposable data on an isolated development host or network:

- The guest serves cleartext HTTP on **all IPv4 interfaces, port 18100**,
  not just loopback. The example has no authentication. Prevent access from
  untrusted networks with a host firewall or network isolation **before**
  starting it. A reverse proxy alone does not close direct access to 18100.
- `-S inherit-network=y` grants broad host-network access; it is not a
  localhost-only or inbound-only grant. Host firewall rules or a custom
  embedding must enforce narrower network policy.
- Run exactly one writer per state directory. Each process loads its own
  in-memory database and overwrites the entire blob after a mutation; two
  replicas sharing the file can lose each other's updates.
- Blob writes use `fopen(..., "wb")` and `fwrite`, not atomic replacement or
  `fsync`. A completed POST is not a power-loss durability guarantee. A
  persistence error can leave the in-memory insert applied and the file
  truncated; retrying POST can duplicate an insert.
- Startup can fall back to an empty database when the file is absent or
  cannot be loaded. Do not use `GET /` as proof of durable-data integrity.
  If expected data is missing, stop writes and investigate the blob/backup.

Cloudflare Workers is **not** a drop-in host for this binary. It is a
`wasi:cli/run` command that binds sockets and uses WASI filesystem calls.
`wasmtime serve` expects `wasi:http/proxy`, a different interface. A Workers
port needs a host adapter and a different request/persistence boundary; the
blob serialization APIs alone do not supply one.

## 1. Build a known revision

Install Git, make, curl, Python 3, wasi-sdk with `wasm32-wasip2` support,
and Wasmtime with component-model and `wasi:sockets` support. Obtain tools
from the official [wasi-sdk releases](https://github.com/WebAssembly/wasi-sdk/releases)
and [Wasmtime releases](https://github.com/bytecodealliance/wasmtime/releases),
verify the release artifacts, and choose the host architecture correctly.
Use a maintained, security-patched Wasmtime release for any real deployment.
The repository's CI compatibility baseline is wasi-sdk 25.0 / Wasmtime 25.0.0;
the commands below were also exercised with wasi-sdk 25.0 / Wasmtime 48.0.2
on macOS arm64.

In a fresh checkout, select the reviewed `dev` commit you intend to deploy
(the release branch does not necessarily contain this example):

```sh
git clone --branch dev https://github.com/c4punks/CWIST.git cwist-appliance-src
cd cwist-appliance-src
git checkout --detach <reviewed-dev-commit>
git submodule update --init --recursive
```

Replace the tool paths below with absolute paths on your host. Run this
and the remaining shell snippets in the same shell, from the checkout root:

```sh
export WASI_SDK=/absolute/path/to/wasi-sdk
export WASMTIME=/absolute/path/to/wasmtime
"$WASI_SDK/bin/clang" --version
"$WASMTIME" --version
make -j4 libcwist_wasip2.a
make wasip2-smoke
./example/wasip2-kv/build.sh
```

Use a clean checkout when changing the SDK, source revision, or compiler flags.
`build.sh` reuses an existing library archive rather than rebuilding it.
It links the guest with `-Wl,-z,stack-size=1048576`; retain this flag when
writing your own link command. This is the guest C stack in linear memory,
not Wasmtime's native stack setting. See the [WASI reference](../api/wasi.md).

`make wasip2-smoke` uses port 18099. Ensure both 18099 and the example's
18100 are unused. The example's port is compiled into `app.c` as `KV_PORT`;
setting a host `PORT` environment variable does not reconfigure the binary.

## 2. Separate the artifact from state

For this disposable walkthrough, create a fresh private appliance directory.
For a long-lived appliance, provision an equivalent directory owned by a
non-root service account on persistent storage instead of a temporary path.

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

Only `state` is preopened to the guest, under the name `kv`. The module and
logs stay outside that filesystem grant. The host reads the module by its
absolute path; the guest does not need access to the source or release directory.
Record the source revision, tool versions, and artifact digest with a release:

```sh
python3 - "$MODULE" <<'PY'
import hashlib, pathlib, sys
p = pathlib.Path(sys.argv[1])
print(hashlib.sha256(p.read_bytes()).hexdigest(), p)
PY
```

## 3. Start and verify

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

Wasmtime flags must precede the module path. `HOST_DIR::GUEST_DIR` makes
`state/cwist.db` on the host appear as `kv/cwist.db` in the guest. Disabling
UDP and DNS does not restrict the TCP grant. This uses `wasmtime run`, not
`serve`. The PID is the host Wasmtime process, not a guest signal target.

Wait for startup, then confirm the process did not exit (for example because
another service already owns the port). Stop on failure and read the log:

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

On a fresh state directory, `/items` returns `[]`. If the final check fails,
do not continue with POST. Inspect `$APPLIANCE/logs/server.log`, correct the
failure, and repeat startup. A readiness response proves only current query
availability, not backup validity or crash durability.

```sh
curl --fail --show-error --silent --max-time 5 \
    -H 'Content-Type: application/json' \
    -d '{"name":"edge","qty":7}' http://127.0.0.1:18100/items
curl --fail --show-error --silent --max-time 5 http://127.0.0.1:18100/items
```

The POST returns `{"ok":true,"id":1}` for a fresh database. The list contains
`{"id":"1","name":"edge","qty":"7"}`: this example's query callback
encodes SQLite values as JSON strings. Check the status and returned content;
do not blindly retry a failed POST.

## 4. Restart, back up, and restore

Stop incoming writes before stopping the process. Stopping Wasmtime does not
invoke a graceful guest shutdown or flush an in-flight mutation. The example
writes its blob during POST; stop only after requests have finished.

```sh
stop
test -s "$APPLIANCE/state/cwist.db"
BACKUP="$APPLIANCE/cwist.db.backup"
cp "$APPLIANCE/state/cwist.db" "$BACKUP"
start
```

Repeat the readiness check and `GET /items` above. The same row must return.
Do not run `example/wasip2-kv/smoke.sh` against valuable example data: it
**deletes its own `kv/cwist.db`** for a clean slate. It also reuses an existing
`app.wasm`. The separate appliance state is not that test directory.

To rehearse restoration without overwriting the active state or its backup,
stop the process and restore into a new appliance directory:

```sh
stop
RESTORE=$(mktemp -d "${TMPDIR:-/tmp}/cwist-restore.XXXXXX")
mkdir -p "$RESTORE/state" "$RESTORE/logs"
cp "$BACKUP" "$RESTORE/state/cwist.db"
APPLIANCE="$RESTORE"
start
```

`MODULE` still names the original release artifact. Repeat readiness and
`GET /items`, compare the returned rows with the expected data, and call
`stop` when finished. The backup and original state remain untouched.

For an upgrade, first stop writes and the old process, preserve an offline
backup, and select a separately built and verified `MODULE`. Start one process
against the intended state and verify the records. Roll back the module only
if its schema is compatible; otherwise restore the matching backup into a
new state directory and explicitly accept losing writes made after that backup.
Never start old and new versions concurrently on the same blob.

## Before adapting this for production

Provide authentication, TLS termination, request limits, protected ingress,
least-privilege network policy, resource limits, monitoring, and host process
supervision. The database persistence adapter also needs atomic writes,
error propagation, a fail-closed load policy, and a defined crash-recovery
contract. Backup retention and schema migration are host/application concerns.
The current example does not implement those controls.

For runtime option semantics, see the official [Wasmtime CLI documentation](https://docs.wasmtime.dev/cli-options.html)
and the installed binary's `wasmtime run --help` / `wasmtime run -S help`.
