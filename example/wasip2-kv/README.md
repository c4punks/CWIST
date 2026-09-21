# CWIST wasip2 edge-KV demo

Edge persistence pattern for `cwist_db` on WASI 0.2 (issue #201 Phase 1): the
guest keeps the database in memory, and the **host owns persistence** as a
single opaque blob — the same shape as a KV namespace on an edge runtime.

```
 guest (app.wasm)                    host
┌───────────────────────┐      ┌──────────────────┐
│ cwist_db (:memory:)   │      │ kv/cwist.db blob │
│  boot: open_memory()  │◀─────│  (preopened dir) │   KV get
│  mutation: serialize()│─────▶│                  │   KV put
└───────────────────────┘      └──────────────────┘
```

- `cwist_db_open_memory(buf, len)` on boot = the KV `get`.
- `cwist_db_serialize()` + `fwrite` after each mutation = the KV `put`.

On Wasmtime the "KV" is a file under a `--dir` preopen. This is not a
Cloudflare Workers adapter: the guest directly uses WASI filesystem and
socket APIs. A Workers port would need host JS persistence and a different
request boundary; it cannot run this binary unchanged.

For separate artifact/state directories, startup, backup, and rollback, see
the [Wasmtime appliance deployment guide](../../docs/deployment/wasmtime-appliance.md).
The example is unauthenticated, binds all IPv4 interfaces, and does not
promise atomic or crash-durable blob writes. Use disposable data on an
isolated host/network. `smoke.sh` deletes the example's `kv/cwist.db`.

## Files

| file | role |
|---|---|
| `app.c` | CWIST app: `GET /`, `GET /items`, `POST /items` + blob round trip |
| `build.sh` | compiles `app.c` + `libcwist_wasip2.a` to `app.wasm` (wasi-sdk) |
| `smoke.sh` | restart-persistence proof: POST → kill → restart → GET |

## Run

Requires wasi-sdk (`WASI_SDK`, default `~/toolchains/wasi-sdk-25.0-x86_64-linux`)
and wasmtime (`WASMTIME`, default `wasmtime` on PATH):

```sh
./build.sh
./smoke.sh   # PASS = the row survives a wasmtime restart
```

`build.sh` links with `-Wl,-z,stack-size=1M`: the socket request path exceeds
wasm-ld's 64KB default stack, and under WASI the overflow silently corrupts
linear memory instead of trapping. Keep that flag for any wasip2 binary that
serves sockets.

Or manually:

```sh
mkdir -p kv
wasmtime run -S preview2=y -S tcp=y -S inherit-network=y --dir . app.wasm &
curl -X POST localhost:18100/items -d '{"name":"edge","qty":7}'
kill %1                                        # blob now at kv/cwist.db
wasmtime run -S preview2=y -S tcp=y -S inherit-network=y --dir . app.wasm &
curl localhost:18100/items                     # row is back
```
