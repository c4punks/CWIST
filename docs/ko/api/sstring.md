# SString API

*헤더:* `<cwist/core/sstring/sstring.h>`

안전한 동적 문자열 처리 라이브러리입니다.

### `cwist_sstring_create`
```c
cwist_sstring *cwist_sstring_create(void);
```
동적 문자열을 만듭니다.

### `cwist_sstring_assign_len`
```c
cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);
```
바이너리 안전 대입입니다. 정확히 `len`바이트를 복사하고 (편의를 위해 끝에 NUL을 덧붙임), 덕분에 HTTP 핸들러가 `\0`이 들어 있는 POST 본문도 저장할 수 있습니다.

### `cwist_sstring_append`
```c
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *suffix);
```
안전한 이어 붙이기입니다. `cwist_error_t`를 반환합니다.

### `cwist_sstring_append_len`
```c
cwist_error_t cwist_sstring_append_len(cwist_sstring *str, const char *data, size_t len);
```
바이너리 안전 덧붙이기입니다. HTTP 응답 직렬화기가 정확한 `Content-Length` 바이트를 스트리밍할 때 사용합니다.

### `cwist_sstring_assign`
```c
cwist_error_t cwist_sstring_assign(cwist_sstring *str, const char *data);
```
문자열 내용을 NUL로 끝나는 C 문자열로 바꿉니다.

### `cwist_sstring_ltrim` / `cwist_sstring_rtrim` / `cwist_sstring_trim`
```c
cwist_error_t cwist_sstring_ltrim(cwist_sstring *str);
cwist_error_t cwist_sstring_rtrim(cwist_sstring *str);
cwist_error_t cwist_sstring_trim(cwist_sstring *str);
```
앞쪽 공백(`ltrim`), 뒤쪽 공백(`rtrim`), 또는 양쪽 모두(`trim`)를 제자리에서 제거합니다.

### `cwist_sstring_compare`
```c
int cwist_sstring_compare(cwist_sstring *str, const char *compare_to);
```
문자열 내용을 NUL로 끝나는 C 문자열과 비교합니다. 같으면 0, `str`이 `compare_to`보다 앞서면 음수, 뒤면 양수를 반환합니다 (`strcmp`와 같은 의미).

### `cwist_sstring_destroy`
```c
void cwist_sstring_destroy(cwist_sstring *str);
```
문자열 메모리를 해제합니다.
