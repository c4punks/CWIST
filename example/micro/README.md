# micro examples

Seventeen minimal, single-file programs — each shows one CWIST feature
in as few lines as possible. Steps 01–14 are plain HTTP servers on port
8080 (run one at a time); steps 15 and 17 serve HTTPS on port 8443;
steps 06–08, 13, and 16 are console programs that print and exit.

| Step | Shows |
|------|-------|
| [`01-hello`](01-hello/main.c) | smallest possible server: one `cwist_app_get`, one `cwist_app_listen` |
| [`02-routes`](02-routes/main.c) | `cwist_app_get` and `cwist_app_post` on the same path |
| [`03-path-params`](03-path-params/main.c) | route parameter `:id` read with `cwist_query_map_get(req->path_params, ...)` |
| [`04-query-params`](04-query-params/main.c) | URL query parameters with defaults |
| [`05-json`](05-json/main.c) | parse a JSON body with cJSON, reply built with `cwist_json_builder` |
| [`06-orm-insert`](06-orm-insert/main.c) | one `cwist_orm_insert` against an in-memory SQLite |
| [`07-orm-query`](07-orm-query/main.c) | `cwist_orm_select` and iterate the returned cJSON rows |
| [`08-orm-update-delete`](08-orm-update-delete/main.c) | `cwist_orm_update` and `cwist_orm_delete` with a WHERE clause |
| [`09-html-builder`](09-html-builder/main.c) | dynamic HTML page with `cwist_html_element_*` and generated CSS |
| [`10-static-files`](10-static-files/main.c) | `cwist_app_static` serving a directory |
| [`11-jwt-auth`](11-jwt-auth/main.c) | issue a JWT with `cwist_jwt_sign`, verify it with `cwist_jwt_verify` |
| [`12-middleware`](12-middleware/main.c) | a logging middleware chained with `cwist_app_use` |
| [`13-nuke-db`](13-nuke-db/main.c) | in-memory SQLite synced to disk with `cwist_app_use_nuke_db` |
| [`14-websocket`](14-websocket/main.c) | WebSocket echo server with `cwist_app_ws` |
| [`15-pqc-tls`](15-pqc-tls/main.c) | HTTPS plus the post-quantum hybrid layer in two lines |
| [`16-rdbms-auto`](16-rdbms-auto/main.c) | `cwist_app_auto_rdbms` probing localhost for PostgreSQL / MySQL / MariaDB |
| [`17-blog-crud`](17-blog-crud/main.c) | full blog combining all of the above: ORM, JSON API, HTML, JWT, middleware, static files, PQC TLS, metrics, healthz |
| [`18-https-hello`](18-https-hello/main.c) | minimal HTTPS server (`cwist_app_use_https`); needs `server.crt`/`server.key` next to the binary |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build all seventeen at once:

```sh
make micro-examples
```

Or build a single step through its root-Makefile target:

```sh
make example/micro/01-hello/hello
```

`make examples-check` builds every example in the repository, including
all micro steps.

## Run

### 01–04: routing basics

```sh
./example/micro/01-hello/hello &
curl http://localhost:8080/
# Hello, CWIST!

./example/micro/02-routes/routes &
curl http://localhost:8080/posts      # LIST all posts
curl -X POST http://localhost:8080/posts   # CREATE a post

./example/micro/03-path-params/path-params &
curl http://localhost:8080/posts/42   # Post ID: 42

./example/micro/04-query-params/query-params &
curl 'http://localhost:8080/posts?page=3&limit=5'   # page=3  limit=5
```

Every server in this section listens on port 8080, so stop one before
starting the next.

### 05: JSON echo

```sh
./example/micro/05-json/json &
curl -X POST http://localhost:8080/echo -d '{"name":"carol"}'
# {"received":"carol"}
```

### 06–08: ORM console programs

These spawn a SQLite worker behind a Unix socket, run their statements,
print a result, and exit. Nothing listens on a port.

```sh
./example/micro/06-orm-insert/orm-insert     # Inserted 1 row
./example/micro/07-orm-query/orm-query       # First / Second
./example/micro/08-orm-update-delete/orm-update-delete   # Updated row 1 / Deleted row 1
```

### 09: HTML builder

```sh
./example/micro/09-html-builder/html-builder &
curl http://localhost:8080/   # text/html page: <h1>CWIST Blog</h1> + generated CSS
```

### 10: static files

`cwist_app_static(app, "/static", "public")` serves the `public`
directory **relative to the current working directory**, so create it
before starting the server:

```sh
mkdir -p public && echo 'hello static' > public/hello.txt
./example/micro/10-static-files/static-files &
curl http://localhost:8080/static/hello.txt   # hello static
```

### 11: JWT auth

`POST /login` returns a signed token; `GET /dashboard` requires it as a
`Bearer` token and greets the `user` claim.

```sh
./example/micro/11-jwt-auth/jwt-auth &
TOKEN=$(curl -s -X POST http://localhost:8080/login | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl http://localhost:8080/dashboard                                   # 401 Unauthorized
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/dashboard # Welcome, alice
```

### 12: middleware

```sh
./example/micro/12-middleware/middleware &
curl http://localhost:8080/    # Hello!  (and "[LOG] GET /" printed by the server)
```

### 13: nuke-db

`cwist_app_use_nuke_db(app, "blog.db", 5000)` keeps the database in
memory and syncs it to `blog.db` on disk every 5 seconds. The file is
created in the current working directory; run it from a scratch
directory if you do not want a `blog.db` in the repository root.

```sh
mkdir /tmp/nuke-demo && cd /tmp/nuke-demo
/path/to/cwist/example/micro/13-nuke-db/nuke-db   # Nuke DB ready...
ls -l blog.db
```

### 14: WebSocket echo

```sh
./example/micro/14-websocket/websocket &
```

Connect a WebSocket client to `ws://localhost:8080/ws`; every text frame
is echoed back prefixed with `Echo: `. The handshake can be checked with
curl (expect `101 Switching Protocols`):

```sh
curl -i -N -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
     -H 'Sec-WebSocket-Version: 13' \
     -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' http://localhost:8080/ws
```

### 15: PQC TLS

`cwist_app_use_https` plus `cwist_app_use_pqc_layer` enables TLS 1.3
with the X25519MLKEM768 hybrid key exchange. The certificate and key
are loaded from the current working directory:

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
     -days 365 -nodes -subj '/CN=localhost'
./example/micro/15-pqc-tls/pqc-tls &
curl -k https://localhost:8443/   # Quantum-safe!
```

### 16: RDBMS auto-mount

```sh
./example/micro/16-rdbms-auto/rdbms-auto
# RDBMS mounted on port 5432   (when PostgreSQL/MySQL/MariaDB is listening there)
# No RDBMS detected on port 5432   (otherwise)
```

### 17: blog CRUD

Combines every feature: SQLite + ORM storage, an HTML index, a JSON
API, JWT login, a logging middleware, static files, the PQC TLS layer,
and the `/metrics` and `/healthz` endpoints. Like step 15 it needs
`server.crt` / `server.key` in the working directory, and it serves
`public/` at `/static/` if that directory exists.

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
     -days 365 -nodes -subj '/CN=localhost'
mkdir -p public
./example/micro/17-blog-crud/blog-crud &   # Blog server on https://localhost:8443

curl -sk https://localhost:8443/                    # HTML index
curl -sk https://localhost:8443/api/posts           # {"posts": []}
TOKEN=$(curl -sk -X POST https://localhost:8443/login | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl -sk -X POST https://localhost:8443/api/posts -d '{"title":"Hi","body":"Body"}'
curl -sk https://localhost:8443/api/posts/1
curl -sk -X POST https://localhost:8443/api/posts/1/edit -d '{"title":"New"}'
curl -sk -X POST https://localhost:8443/api/posts/1/delete
curl -sk https://localhost:8443/healthz             # {"status":"ok"}
curl -sk https://localhost:8443/metrics             # Prometheus text format
```

## Clean

```sh
make clean-examples   # removes every root-Makefile example binary, including all micro steps
```
