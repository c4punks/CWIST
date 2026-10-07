# JSON 빌더 API

JSON 빌더는 완전한 DOM 파서의 부담 없이 JSON 문자열을 만드는 가벼운 방법을 제공합니다.

## 함수

### `cwist_json_builder_create`
```c
cwist_json_builder *cwist_json_builder_create(void);
```

### `cwist_json_begin_object` / `cwist_json_end_object`
JSON 객체 `{ ... }`를 시작하고 끝냅니다.

### `cwist_json_add_string`
값이 문자열인 키-값 쌍을 추가합니다.
```c
void cwist_json_add_string(cwist_json_builder *b, const char *key, const char *value);
```

### `cwist_json_add_int`
값이 정수인 키-값 쌍을 추가합니다.
```c
void cwist_json_add_int(cwist_json_builder *b, const char *key, int value);
```

### `cwist_json_add_bool`
값이 불리언인 키-값 쌍을 추가합니다.
```c
void cwist_json_add_bool(cwist_json_builder *b, const char *key, bool value);
```

### `cwist_json_add_null`
값이 null인 키-값 쌍을 추가합니다.
```c
void cwist_json_add_null(cwist_json_builder *b, const char *key);
```

### `cwist_json_begin_array` / `cwist_json_end_array`
JSON 배열 `[ ... ]`을 시작하고 끝냅니다. `key`를 주면 `"key": [` 형태로 추가합니다.
```c
void cwist_json_begin_array(cwist_json_builder *b, const char *key);
void cwist_json_end_array(cwist_json_builder *b);
```

### `cwist_json_get_raw`
만들어진 JSON 문자열을 `const char *`로 반환합니다.
```c
const char *cwist_json_get_raw(cwist_json_builder *b);
```
- **소유권:** 문자열은 빌더가 소유하며 빌더가 파괴될 때까지 유효합니다. 이 포인터를 해제하지 마세요.

### `cwist_json_builder_destroy`
```c
void cwist_json_builder_destroy(cwist_json_builder *b);
```
`cwist_json_get_raw`가 반환한 내부 버퍼를 포함해 빌더가 소유한 메모리를 모두 해제합니다. 빌더가 더 이상 필요 없을 때 한 번 호출해야 합니다.

## 예제
```c
cwist_json_builder *jb = cwist_json_builder_create();
cwist_json_begin_object(jb);
cwist_json_add_string(jb, "status", "success");
cwist_json_add_int(jb, "code", 200);
cwist_json_end_object(jb);

printf("%s\n", cwist_json_get_raw(jb));

cwist_json_builder_destroy(jb);
```
