# Document an API with OpenAPI and Swagger UI

This guide shows how to describe routes with `CWIST_OPENAPI_*` annotations,
generate an `openapi.json` file with the `cwist openapi` command, and serve it
next to an interactive Swagger UI page with `cwist_app_enable_swagger()`.

## 1. Annotate the routes

Declare each route with a `CWIST_OPENAPI_<METHOD>` macro from
`<cwist/openapi.h>` instead of the matching `cwist_app_<method>` call. The
macros (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`) expand to the normal routing
call, so the server behaves the same. They exist so that `cwist openapi` can find
the route in the C source.

Put a Doxygen comment (`/** ... */`) directly above the macro to describe the
operation:

```c
#include <cwist/sys/app/app.h>
#include <cwist/openapi.h>

static void list_users(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "[{\"id\":1,\"name\":\"ada\"}]");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

static void get_user(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"id\":1,\"name\":\"ada\"}");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

int main(void) {
    cwist_app *app = cwist_app_create();

    /** @openapi.summary List users
     *  @openapi.description Returns all active users.
     *  @openapi.tags users,public
     *  @openapi.response 200 application/json User collection.
     *  @openapi.response 500 application/json Internal error. */
    CWIST_OPENAPI_GET(app, "/users", list_users);

    /** @openapi.summary Get one user
     *  @openapi.operation_id getUser
     *  @openapi.response 200 application/json The user.
     *  @openapi.response 404 application/json No such user. */
    CWIST_OPENAPI_GET(app, "/users/:id", get_user);

    cwist_app_enable_swagger(app, "/docs", "openapi.json");
    int rc = cwist_app_listen(app, 8099);
    cwist_app_destroy(app);
    return rc;
}
```

Build it like any other CWIST program (see the [README](../../README.md)).

### Annotation tags

Each tag goes on its own line of the comment.

| Tag | Effect in `openapi.json` |
|-----|--------------------------|
| `@openapi.summary <text>` | `summary` |
| `@openapi.description <text>` | `description` |
| `@openapi.operation_id <id>` | `operationId` |
| `@openapi.tags a,b` | `tags`, split on commas |
| `@openapi.response <code> <media-type> <text>` | one entry under `responses` |

Notes on how the tool reads the source:

* The comment used for a route is the closest `/** ... */` comment above the
  macro. A route with no `@openapi.` tags in that comment still appears, with a
  single `200` response described as "Success".
* In `@openapi.response`, the second word is always read as the media type. Write
  one (for example `application/json`) on every line: `500 Internal error.`
  would be emitted as `"description": "error."`, because `Internal` is taken as
  the media type and dropped. A media type produces a `content` entry whose
  schema is `{"type": "object"}`; no request or response schemas are generated.
* A `:name` segment in the route path becomes `{name}` and is listed as a
  required string path parameter, so `/users/:id` is emitted as `/users/{id}`.

## 2. Generate `openapi.json`

`cwist openapi` takes a source file or a directory (searched recursively for
`*.c`) and writes an OpenAPI 3.1 document. Run it from the project directory:

```sh
cwist openapi src --title "Users API" --version 1.0.0
```

```
wrote openapi.json (2 paths)
```

| Option | Meaning |
|--------|---------|
| `--output <file>` | Where to write the document. Default: `openapi.json` |
| `--title <text>` | `info.title`. Default: `CWIST API` |
| `--version <text>` | `info.version`. Default: `0.1.0` |
| `--project <file>` | A `.cwpro` project file whose `openapi` section supplies the defaults above |

The script is `tools/cli/cwist` in the source tree, and `make install` installs
it as `cwist`. From a source checkout you can run `python3 tools/cli/cwist
openapi ...` instead.

An excerpt of the result for the program above:

```json
{
  "openapi": "3.1.0",
  "info": { "title": "Users API", "version": "1.0.0" },
  "paths": {
    "/users/{id}": {
      "get": {
        "summary": "Get one user",
        "operationId": "getUser",
        "parameters": [
          { "name": "id", "in": "path", "required": true, "schema": { "type": "string" } }
        ],
        "responses": {
          "200": { "description": "The user.", "content": { "application/json": { "schema": { "type": "object" } } } },
          "404": { "description": "No such user.", "content": { "application/json": { "schema": { "type": "object" } } } }
        }
      }
    }
  }
}
```

Re-run the command whenever the annotations change. The server does not
generate the file itself.

## 3. Serve the UI

```c
void cwist_app_enable_swagger(cwist_app *app, const char *mount_path,
                              const char *openapi_json_path);
```

`cwist_app_enable_swagger(app, "/docs", "openapi.json")` registers two GET
routes:

* `mount_path` (`"/docs"` here, and also the default when `NULL` is passed) serves a
  small HTML page that loads Swagger UI.
* `/openapi.json` serves the file at `openapi_json_path`. The file is read from
  disk on every request, so regenerating it takes effect without a restart. The
  path is resolved against the working directory of the server process, and a
  missing file produces a `404` with `{"error":"openapi.json not found"}`.

Start the server from the directory that contains `openapi.json` and check both
routes:

```sh
./users &
curl -i http://127.0.0.1:8099/docs
curl -i http://127.0.0.1:8099/openapi.json
```

Both return `200 OK`; `/docs` is `text/html` and `/openapi.json` is
`application/json`. Open `http://127.0.0.1:8099/docs` in a browser to try the
operations.

Things to know:

* The HTML page always asks for `/openapi.json`, whatever `mount_path` is.
* The page loads the Swagger UI scripts and stylesheet from
  `https://unpkg.com/swagger-ui-dist@5/`, so the browser needs internet access
  to render it. The server itself does not.
* The path is stored in one process-wide buffer, so a second call replaces the
  JSON path used by every app in the process.
