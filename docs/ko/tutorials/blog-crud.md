# CWIST로 CRUD 블로그 만들기

이 가이드는 CWIST의 SQLite 연동을 바탕으로 작은 JSON 블로그 API를 만듭니다. 요청, 검증, 응답의
경계를 쉽게 살펴볼 수 있도록 SQL을 의도적으로 명시적으로 둡니다.

## 1. 애플리케이션 만들기

```c
#include <cwist/sys/app/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/net/http/http.h>

static void list_posts(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Query through your existing cwist_db handle and serialize rows as JSON. */
    cwist_sstring_assign(res->body, "[]");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_db(app, "blog.db");
    cwist_app_get(app, "/posts", list_posts);
    int rc = cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return rc;
}
```

애플리케이션 시작 시 마이그레이션으로 테이블을 만듭니다.

```sql
CREATE TABLE IF NOT EXISTS posts (
  id INTEGER PRIMARY KEY,
  title TEXT NOT NULL,
  body TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

## 2. 라우트 정의하기

작업마다 핸들러 하나를 두고, 글 식별자에는 파라미터가 있는 라우트를 씁니다.

| 메서드 | 경로 | 동작 |
| --- | --- | --- |
| GET | `/posts` | 글 목록 |
| GET | `/posts/:id` | 글 하나 읽기 |
| POST | `/posts` | 글 만들기 |
| PATCH | `/posts/:id` | 제목/본문 수정 |
| DELETE | `/posts/:id` | 글 삭제 |

```c
cwist_app_get(app, "/posts", list_posts);
cwist_app_get(app, "/posts/:id", get_post);
cwist_app_post(app, "/posts", create_post);
cwist_app_patch(app, "/posts/:id", update_post);
cwist_app_delete(app, "/posts/:id", delete_post);
```

라우트 파라미터는 `cwist_query_map_get(req->path_params, "id")`로 읽습니다. 신뢰할 수 없는
입력으로 다루세요. 정수 전체를 파싱하고, 0 이하의 값은 거부하고, prepared statement에
바인딩하세요. JSON 필드나 라우트 파라미터를 SQL 문자열에 끼워 넣으면 절대 안 됩니다.

## 3. JSON을 검증하고 HTTP 의미에 맞게 응답하기

만들 때는 `title`과 `body` 문자열을 요구하세요. 삽입 후에는 `201 Created`, 삭제에 성공하면
`204 No Content`, 잘못된 요청에는 `400 Bad Request`, id에 해당하는 행이 없으면
`404 Not Found`를 반환합니다. JSON 응답에는 항상 `Content-Type: application/json`을 설정하세요.

브라우저 클라이언트를 위해서는 직접 관리하는 출처에만 CORS를 추가하고, 쿠키 인증을 켰다면 CSRF
미들웨어를 쓰세요. 기존 WAF 미들웨어는 유용한 추가 경계이지만, 파라미터화된 SQL을 대신하지는
않습니다.

## 4. API 테스트하기

포트를 바인딩하지 않고 요청을 디스패치하려면 `cwist_test_client`를 쓰세요. 만들기, 목록, 조회,
수정, 삭제, 그리고 다시 조회해서 404를 단언하는 전체 수명 주기를 다루세요. 빠른 격리를 위해서는
`:memory:`에서, 마이그레이션을 실험하기 위해서는 임시 디스크 데이터베이스에서 같은 테스트를
실행하세요.
