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

On wasmtime the "KV" is a file under a `--dir` preopen. On Cloudflare Workers
the same two calls map to `KV.put(key, blob)` / `KV.get(key, "arrayBuffer")`
from the host JS — the guest code is unchanged either way.

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
