# 쿼리와 URI API

*헤더:* `<cwist/net/http/query.h>`

`liburiparser`를 사용하는 고급 쿼리 문자열 파싱입니다.

### `cwist_query_map_parse`
```c
void cwist_query_map_parse(cwist_query_map *map, const char *raw_query);
```
쿼리 문자열(예: `a=1&b=2`)을 `liburiparser`로 파싱해 맵에 넣습니다.

### `cwist_query_map_get`
```c
const char *cwist_query_map_get(cwist_query_map *map, const char *key);
```
쿼리 파라미터 값을 평균 O(1) 시간에 가져옵니다 (SipHash).
