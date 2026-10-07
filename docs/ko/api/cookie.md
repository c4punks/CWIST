# 쿠키 API

*헤더:* `<cwist/net/http/cookie.h>`

요청의 `Cookie` 헤더를 읽고 `Set-Cookie` 응답 헤더를 쓰기 위한 도우미입니다. 파싱된 쿠키는
`cwist_query_map`에 저장됩니다 ([쿼리와 URI API](query.md) 참고).

## 쿠키 읽기

### `cwist_cookie_parse`
```c
void cwist_cookie_parse(cwist_query_map *map, const char *header);
```
`name1=value1; name2=value2` 형식의 `Cookie` 헤더 값을 파싱해 각 쌍을 `map`에 저장합니다.
`map`은 `cwist_query_map_create()`로 만든 것이어야 합니다.

* 이름 앞의 공백과 이름 뒤의 공백은 제거합니다. 값 끝의 공백은 그대로 둡니다.
* 각 값은 `cwist_cookie_decode`로 URL 디코딩합니다.
* `=`가 없거나, 이름이 비어 있거나, 디코딩한 값이 4096바이트에 들어가지 않는 쌍은 건너뜁니다.
* `NULL`이거나 빈 `header`, 또는 `NULL` `map`이면 아무것도 하지 않습니다.

### `cwist_cookie_get`
```c
const char *cwist_cookie_get(cwist_query_map *map, const char *name);
```
`name`에 저장된 디코딩된 값을 반환하며, 없으면 `NULL`입니다. 포인터는 `map`에 속하며 맵이
파괴될 때까지 유효합니다.

## 쿠키 쓰기

### `cwist_cookie_options`
```c
typedef struct cwist_cookie_options {
    const char *path;
    const char *domain;
    int max_age_seconds;   /* < 0 means omit */
    bool http_only;
    bool secure;
    const char *same_site; /* "Strict", "Lax", or "None" */
} cwist_cookie_options;
```
`cwist_cookie_set`의 속성입니다. `NULL`인 `path`, `domain`, `same_site`는 생략되고,
`http_only`와 `secure`는 `true`일 때만 추가됩니다.

`max_age_seconds`는 `0` 이상이면 항상 기록되므로, 0으로 초기화한 구조체는 `Max-Age=0`을
만들어 클라이언트에게 쿠키 삭제를 지시합니다. 속성을 생략하려면 `-1`로 설정하세요.
`same_site`는 주어진 그대로 복사되며 위 세 값 중 하나인지 검사하지 않습니다.

### `cwist_cookie_set`
```c
int cwist_cookie_set(cwist_http_response *res, const char *name, const char *value,
                     const cwist_cookie_options *opts);
```
`name=<encoded value>; Path=...; Domain=...; Max-Age=...; HttpOnly; Secure; SameSite=...`
형식의 `Set-Cookie` 헤더를 `res`에 추가합니다. 값은 `cwist_cookie_encode`로 인코딩하고, 이름과
속성 문자열은 주어진 그대로 씁니다. `NULL` `value`는 빈 값으로 보내고, `NULL` `opts`는
`name=value`만 보냅니다.

성공하면 `0`, 실패하면 `-1`을 반환합니다. 실패 조건은 `res`나 `name`이 `NULL`인 경우, 할당
실패, CR 또는 LF 문자가 들어 있어 헤더가 거부된 경우입니다.

### `cwist_cookie_delete`
```c
int cwist_cookie_delete(cwist_http_response *res, const char *name);
```
클라이언트에게 쿠키 삭제를 지시하는 `Set-Cookie: name=; Path=/; Max-Age=0` 헤더를 추가합니다.
경로가 `/`로 고정되어 있으므로, 다른 `path`로 설정된 쿠키는 이 헤더로 지워지지 않습니다.
성공하면 `0`, 실패하면 `-1`을 반환합니다.

## 인코딩

### `cwist_cookie_encode`
```c
char *cwist_cookie_encode(const char *value);
```
`value`를 퍼센트 인코딩합니다. `A-Z a-z 0-9 - _ . ~` 문자는 그대로 두고, 나머지 바이트는 모두
대문자 16진수 `%XX`로 바꿉니다 (공백은 `%20`). 호출자가 `cwist_free()`로 해제하는 힙 문자열을
반환하며, `value`가 `NULL`이거나 할당에 실패하면 `NULL`입니다.

### `cwist_cookie_decode`
```c
int cwist_cookie_decode(const char *in, char *out, size_t out_len);
```
`in`을 크기가 `out_len`인 호출자 버퍼 `out`으로 디코딩합니다. `%XX` 시퀀스(16진수 대소문자
모두)는 해당 바이트가 되고 `+`는 공백이 됩니다. 뒤에 16진수 두 자리가 오지 않는 `%`는 그대로
복사됩니다.

종료 `\0`을 제외하고 쓴 바이트 수를 반환합니다. 인자가 `NULL`이거나, `out_len`이 `0`이거나,
결과가 종료 `\0`과 함께 `out`에 들어가지 않으면 `-1`입니다.

## 예제

요청에서 쿠키를 읽고 응답에 쿠키를 설정합니다.

```c
static void handle_login(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *cookies = cwist_query_map_create();
    cwist_cookie_parse(cookies, cwist_http_header_get(req->headers, "Cookie"));
    const char *theme = cwist_cookie_get(cookies, "theme");  /* NULL if absent */
    (void)theme;
    cwist_query_map_destroy(cookies);

    cwist_cookie_options opts = {
        .path = "/",
        .max_age_seconds = 3600,
        .http_only = true,
        .secure = true,
        .same_site = "Lax",
    };
    cwist_cookie_set(res, "session_id", "abc 123", &opts);
    /* Set-Cookie: session_id=abc%20123; Path=/; Max-Age=3600; HttpOnly; Secure; SameSite=Lax */
}
```

각 함수의 입력과 출력은 `tests/test_cookie.c`에서 확인할 수 있습니다.
