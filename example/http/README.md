# http examples

Four steps that build on each other, each showing one concept from the
CWIST HTTP API.

| Step | Shows |
|------|-------|
| [`step-1-hello-world`](step-1-hello-world/main.c) | minimal server: one handler, one `cwist_app_listen` call |
| [`step-2-mux-router`](step-2-mux-router/main.c) | multiple routes with `cwist_app_get` and `cwist_app_post`, HTML rendered with `cwist_html_builder` |
| [`step-3-query-params`](step-3-query-params/main.c) | read URL query parameters with `cwist_query_map_get` |
| [`step-4-json-api`](step-4-json-api/main.c) | JSON REST API: build a response with `cwist_json_builder`, parse a request body with cJSON |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build a step:

```sh
make -C example/http/step-1-hello-world
make -C example/http/step-2-mux-router
make -C example/http/step-3-query-params
make -C example/http/step-4-json-api
```

`make examples-check` builds all four steps.

## Run

Each step listens on a different port so they can all run at once:

```sh
./example/http/step-1-hello-world/step-1-hello-world   # port 8080
./example/http/step-2-mux-router/step-2-mux-router     # port 8081
./example/http/step-3-query-params/step-3-query-params # port 8082
./example/http/step-4-json-api/step-4-json-api         # port 8083
```

## Step 1: hello world

The shortest valid CWIST server. `cwist_app_get` registers a handler for `GET /`,
`cwist_sstring_assign` sets the response body, and `cwist_app_listen` blocks
until the process is stopped.

```sh
curl http://localhost:8080/
# Hello, World!
```

## Step 2: mux router

Multiple routes on one app. The home handler renders an HTML page with the
HTML builder and CSS composer; `/hello` returns plain text; `/echo` mirrors
the POST body back to the caller.

```sh
curl http://localhost:8081/hello
# Hello from the router!
curl -X POST http://localhost:8081/echo -d 'test body'
# test body
```

## Step 3: query parameters

`req->query_params` is populated automatically. `cwist_query_map_get` looks
up a named parameter by key; it returns `NULL` when the key is absent.

```sh
curl 'http://localhost:8082/search?q=hello&page=2&sort=asc'
```

The response lists the values of `q`, `page`, and `sort`.

## Step 4: JSON REST API

`GET /users` returns a JSON array built with `cwist_json_builder`.
`POST /users` accepts a JSON body, parses it with `cJSON_Parse`, and echoes
the `name` field back with a `status` field.

```sh
curl http://localhost:8083/users
# [{"id":1,"name":"Alice","active":true},{"id":2,"name":"Bob","active":false}]
curl -X POST http://localhost:8083/users \
     -H 'Content-Type: application/json' \
     -d '{"name":"Carol"}'
# {"status":"created","name":"Carol"}
```

## Clean

```sh
make -C example/http/step-1-hello-world clean
make -C example/http/step-2-mux-router clean
make -C example/http/step-3-query-params clean
make -C example/http/step-4-json-api clean
```
