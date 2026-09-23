# CWIST WASM Service Worker demo

End-to-end example app for CWIST on WebAssembly (issue #93 Phase 4): routing,
zod validation, template rendering, `cwist_db`, and signed-cookie sessions
running **inside a Service Worker** - no origin server, no framework JS, no
CDN. The worker intercepts fetches and dispatches them into a CWIST app
compiled to WASM.

```
 browser page        Service Worker                WASM (app.js/app.wasm)
┌────────────┐     ┌───────────────────────┐     ┌──────────────────────────┐
│ index.html │────▶│ importScripts(app.js) │────▶│ CWIST_WASM_DEFINE_ENTRY  │
│  (boot)    │     │  • own cookie jar     │     │  router (mux)            │
└────────────┘     │  • fetch interception │     │  zod validation          │
      │            │  • secret injection   │     │  template rendering      │
      └────────────│  • request/response   │◀────│  cwist_db (:memory:)     │
        GET / POST │    serialization      │     │  sessions (signed cookie)│
                   └───────────────────────┘     └──────────────────────────┘
```

## Files

| file | role |
|---|---|
| `app.c` | the CWIST app; routes, zod schema, page template, db migration |
| `build.sh` | compiles `app.c` + `libcwist_wasm.a` to `app.js`/`app.wasm` (emcc) |
| `sw.js` | Service Worker host: dispatch, cookie jar, session secret, caching |
| `index.html` | static boot shell; registers the worker and reloads into `/` |
| `smoke.js` | node smoke over the same module via the cwist-wasm wrapper |

## Routes (all served from WASM)

- `GET /` - template-rendered page: session visit counter (`{{ visits }}`),
  item count, an add-item form, and the item list via `{% for item in items %}`.
- `POST /items` - JSON body `{"name": string, "qty": int}` validated with zod;
  valid → `201 {"ok":true,"id":N}` inserted into `cwist_db`, invalid → `400`
  with the per-field error list.
- `GET /items` - item rows from `cwist_db` as JSON (values are strings -
  SQLite exec-callback semantics).
- `GET /items/list` - the item list rendered from components with scoped CSS
  (`cwist_html_component_t`, `cwist_css_scope`). A request with `HX-Request:
  true` or `Turbo-Frame` gets only the `<ul id="item-list">` fragment. Any
  other request gets a full page linking `/assets/list.<hash>.css`, a
  bundled and minified stylesheet that the module serves itself
  (`cwist_app_asset_add`), with an immutable cache lifetime. This is the same
  code a native CWIST server runs; see docs/api/html.md.
- `GET /items/image` - the serialized SQLite image (`cwist_db_serialize`):
  the blob an edge host would persist outside the instance and reopen with
  `cwist_db_open_memory`.
- anything else - the router's default `404`.

## Build

Requires the Emscripten toolchain and a built `libcwist_wasm.a`:

```sh
# from the CWIST repo root
make wasm                                   # or: make wasm EMCC=/path/to/emcc
cd example/wasm-service-worker
./build.sh                                  # produces app.js + app.wasm
```

`build.sh` links against `../../libcwist_wasm.a` and exports the standard
entry points (`_cwist_wasm_dispatch`, `_cwist_wasm_dispose`,
`_cwist_wasm_use_session`, `_malloc`, `_free` + `HEAPU8`/`HEAPU32`) with
`-sMODULARIZE -sEXPORT_NAME=createCwistAppModule`, so `sw.js` loads the glue
with `importScripts('./app.js')` and instantiates it itself. `_main` is
exported on purpose: without it the linker drops `main()` and the app (db
migration, routes) is never created.

Artifacts (`app.js`, `app.wasm`) are gitignored; build before serving.

## Run locally

Service Workers require a secure context; `localhost` counts as one. Serve
the directory statically:

```sh
cd example/wasm-service-worker
python3 -m http.server 8080
# open http://localhost:8080/
```

The first load serves the static `index.html`, which registers `sw.js` and
reloads; from then on every same-origin GET/POST (`/`, `/items`, the form
submission) is answered by the CWIST app inside the worker. Static assets
(`index.html`, `sw.js`, `app.js`, `app.wasm`) are precached at install and
served cache-first.

## What to check in the browser (manual verification)

The worker logic is covered by `smoke.js` under node; the Service Worker
plumbing itself needs a browser:

1. Open `http://localhost:8080/` - after the reload you should see the
   CWIST-rendered page ("CWIST inside a Service Worker"), not this boot shell.
2. Reload a few times - "Session visits" increments. The count is carried by
   a signed cookie that `sw.js` stores in its own jar: DevTools > Application >
   Cookies shows nothing, because the worker never forwards `Set-Cookie` to
   the document (it is a forbidden header on constructed SW responses).
3. Add an item via the form - `201` appears, then the page reloads with the
   item listed.
4. Submit an invalid item (empty qty) - the raw `400` JSON with the zod
   error list is shown in the result box.
5. `curl http://localhost:8080/items` (uncontrolled client - bypasses the SW)
   returns the router's 404, proving the responses really come from the worker.
6. DevTools > Application > Service Workers shows the activated worker;
   the Console shows no network requests for page navigations other than the
   static assets.

## Sessions and the cookie jar

Sessions are client-side signed cookies (HMAC-SHA256), so the only state that
must survive a module restart is the signing secret. `sw.js` pins one with
`_cwist_wasm_use_session` before the first dispatch (`CWIST_SESSION_SECRET` at
the top of `sw.js`). **Change it for anything real** - anyone who knows the
secret can forge sessions; the demo value is in the bundle for readability.

A Service Worker does not receive the page's document cookies, so the worker
keeps its own jar: `Set-Cookie` from a dispatch response is captured (and
consumed, not forwarded), and later dispatches get a synthesized `Cookie`
header from the jar. This mirrors what any fetch-interception host (Cloudflare
Worker, etc.) has to do; see "The host carries the cookie" in
`docs/api/wasm.md`.

## Node smoke (no browser needed)

```sh
./build.sh && node smoke.js
```

Drives the same `app.js` through the real cwist-wasm wrapper
(`wasm/npm`, referenced by relative path) and asserts: `GET /` 200 with visit
counter + `Set-Cookie`, visit persistence across requests, `POST /items`
201 / 400 (missing field and wrong type), `GET /items` JSON, the db image
endpoint, session survival across a fresh module instance with the same
pinned secret, and the 404 fallthrough. Also run in CI
(`.github/workflows/wasm.yml`).

## Notes and limits

- The database is `:memory:` inside the WASM instance: items vanish when the
  worker restarts. `GET /items/image` shows the escape hatch - serialize the
  image, persist it in the host (IndexedDB, KV), reopen with
  `cwist_db_open_memory`.
- Only same-origin GET/POST are intercepted; other methods and cross-origin
  requests fall through to the browser default.
- The cookie jar is in-memory per worker: it is gone when the worker is
  destroyed. Persist it (e.g. Cache API / IndexedDB) for a real deployment.
