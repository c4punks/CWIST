# OpenAPI와 Swagger UI로 API 문서화하기

이 가이드는 `CWIST_OPENAPI_*` 주석으로 라우트를 설명하고, `cwist openapi` 명령으로
`openapi.json` 파일을 생성하고, `cwist_app_enable_swagger()`로 대화형 Swagger UI 페이지와 함께
제공하는 방법을 보여 줍니다.

## 1. 라우트에 주석 달기

각 라우트는 대응하는 `cwist_app_<method>` 호출 대신 `<cwist/openapi.h>`의
`CWIST_OPENAPI_<METHOD>` 매크로로 선언합니다. 매크로(`GET`, `POST`, `PUT`, `DELETE`, `PATCH`)는
일반 라우팅 호출로 전개되므로 서버 동작은 같습니다. 이 매크로는 `cwist openapi`가 C 소스에서
라우트를 찾을 수 있도록 하기 위해 존재합니다.

연산을 설명하는 Doxygen 주석(`/** ... */`)을 매크로 바로 위에 둡니다.

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

다른 CWIST 프로그램과 똑같이 빌드합니다 ([README](../../../README.md) 참고).

### 주석 태그

각 태그는 주석의 한 줄에 하나씩 씁니다.

| 태그 | `openapi.json`에서의 효과 |
|-----|--------------------------|
| `@openapi.summary <text>` | `summary` |
| `@openapi.description <text>` | `description` |
| `@openapi.operation_id <id>` | `operationId` |
| `@openapi.tags a,b` | `tags`, 쉼표로 나눔 |
| `@openapi.response <code> <media-type> <text>` | `responses` 아래의 항목 하나 |

도구가 소스를 읽는 방식에 대한 참고 사항:

* 라우트에 쓰이는 주석은 매크로 위에서 가장 가까운 `/** ... */` 주석입니다. 그 주석에
  `@openapi.` 태그가 없는 라우트도 "Success"로 설명된 `200` 응답 하나와 함께 나타납니다.
* `@openapi.response`에서 두 번째 단어는 항상 미디어 타입으로 읽힙니다. 모든 줄에 미디어 타입을
  쓰세요 (예: `application/json`). `500 Internal error.`라고 쓰면 `Internal`이 미디어 타입으로
  처리되어 빠지므로 `"description": "error."`가 출력됩니다. 미디어 타입은 스키마가
  `{"type": "object"}`인 `content` 항목을 만들며, 요청이나 응답 스키마는 생성되지 않습니다.
* 라우트 경로의 `:name` 세그먼트는 `{name}`이 되고 필수 문자열 경로 파라미터로 나열되므로,
  `/users/:id`는 `/users/{id}`로 출력됩니다.

## 2. `openapi.json` 생성하기

`cwist openapi`는 소스 파일이나 디렉터리(`*.c`를 재귀적으로 검색)를 받아 OpenAPI 3.1 문서를
씁니다. 프로젝트 디렉터리에서 실행하세요.

```sh
cwist openapi src --title "Users API" --version 1.0.0
```

```
wrote openapi.json (2 paths)
```

| 옵션 | 의미 |
|--------|---------|
| `--output <file>` | 문서를 쓸 위치. 기본값: `openapi.json` |
| `--title <text>` | `info.title`. 기본값: `CWIST API` |
| `--version <text>` | `info.version`. 기본값: `0.1.0` |
| `--project <file>` | `openapi` 섹션이 위 기본값을 제공하는 `.cwpro` 프로젝트 파일 |

이 스크립트는 소스 트리의 `tools/cli/cwist`이며, `make install`이 `cwist`로 설치합니다. 소스
체크아웃에서는 대신 `python3 tools/cli/cwist openapi ...`로 실행할 수 있습니다.

위 프로그램에 대한 결과의 일부입니다.

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

주석이 바뀔 때마다 명령을 다시 실행하세요. 서버가 파일을 직접 생성하지는 않습니다.

## 3. UI 제공하기

```c
void cwist_app_enable_swagger(cwist_app *app, const char *mount_path,
                              const char *openapi_json_path);
```

`cwist_app_enable_swagger(app, "/docs", "openapi.json")`은 GET 라우트 두 개를 등록합니다.

* `mount_path`(여기서는 `"/docs"`이며, `NULL`을 넘겼을 때의 기본값이기도 합니다)는 Swagger UI를
  불러오는 작은 HTML 페이지를 제공합니다.
* `/openapi.json`은 `openapi_json_path`의 파일을 제공합니다. 파일은 요청마다 디스크에서 읽으므로,
  다시 생성하면 재시작 없이 반영됩니다. 경로는 서버 프로세스의 작업 디렉터리를 기준으로 해석되며,
  파일이 없으면 `{"error":"openapi.json not found"}`와 함께 `404`를 반환합니다.

`openapi.json`이 있는 디렉터리에서 서버를 시작하고 두 라우트를 확인하세요.

```sh
./users &
curl -i http://127.0.0.1:8099/docs
curl -i http://127.0.0.1:8099/openapi.json
```

둘 다 `200 OK`를 반환합니다. `/docs`는 `text/html`이고 `/openapi.json`은 `application/json`입니다.
브라우저에서 `http://127.0.0.1:8099/docs`를 열어 연산을 시험해 보세요.

알아 둘 점:

* HTML 페이지는 `mount_path`와 상관없이 항상 `/openapi.json`을 요청합니다.
* 페이지는 Swagger UI 스크립트와 스타일시트를 `https://unpkg.com/swagger-ui-dist@5/`에서
  불러오므로, 브라우저가 렌더링하려면 인터넷 접근이 필요합니다. 서버 자체는 필요 없습니다.
* 경로는 프로세스 전체에 하나뿐인 버퍼에 저장되므로, 두 번째 호출은 프로세스 안 모든 앱이 쓰는
  JSON 경로를 바꿉니다.
