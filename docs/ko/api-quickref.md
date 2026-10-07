# CWIST API 레퍼런스

## 에러 처리

### `make_error`
- `cwist_error_t make_error(cwist_errtype_t type)`
- 요청한 에러 타입으로 `cwist_error_t`를 만듭니다.

### `cwist_error_is_ok` / `cwist_error_is_ok_extern`
- `static inline bool cwist_error_is_ok(const cwist_error_t *err)`
- `bool cwist_error_is_ok_extern(const cwist_error_t *err)`
- 활성 채널이 0/빈 값을 담고 있으면 참입니다 (NULL도 성공으로 칩니다). `_extern` 형태는
  같은 검사를 실제 심볼로 제공하는 것으로, 헤더에서 생성한 바인딩처럼 `static inline` 함수를
  쓸 수 없는 호출자를 위한 것입니다.

### 그 밖에 내보낸 inline 도우미
- `bool cwist_endpoint_has_extern(cwist_endpoint_opt_t opts, cwist_endpoint_opt_t flag)`는
  `cwist_endpoint_has()`의 out-of-line 버전입니다.
- `include/cwist/`의 모든 `static inline` 도우미는 이런 식으로 내보내지거나, CI가 실행하는
  `scripts/ci/check_inline_exports.py`에 이유와 함께 등록되어 있습니다.

## SString (`cwist_sstring`)

### 수명 주기
- `cwist_sstring *cwist_sstring_create(void)`
- `void cwist_sstring_destroy(cwist_sstring *str)`
- `cwist_error_t cwist_sstring_init(cwist_sstring *str)`

### 핵심 도우미
- `size_t cwist_sstring_get_size(cwist_sstring *str)`
- `cwist_error_t cwist_sstring_change_size(cwist_sstring *str, size_t size, bool blow_data)`
- `cwist_error_t cwist_sstring_assign(cwist_sstring *str, char *data)`

### 공백 제거
- `cwist_error_t cwist_sstring_ltrim(cwist_sstring *str)`
- `cwist_error_t cwist_sstring_rtrim(cwist_sstring *str)`
- `cwist_error_t cwist_sstring_trim(cwist_sstring *str)`

### 덧붙이기 / 복사
- `cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *data)`
- `cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from)`
- `cwist_error_t cwist_sstring_copy(cwist_sstring *origin, char *destination)`
- `cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from)`

### 비교 / 조회
- `int cwist_sstring_compare(cwist_sstring *str, const char *compare_to)`
- `int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right)`
- `cwist_error_t cwist_sstring_seek(cwist_sstring *str, char *substr, int location)`
- `cwist_sstring *cwist_sstring_substr(cwist_sstring *str, int start, int length)`

## HTTP

### 요청 수명 주기
- `cwist_http_request *cwist_http_request_create(void)`
- `void cwist_http_request_destroy(cwist_http_request *req)`
- `cwist_http_request *cwist_http_parse_request(const char *raw_request)`

### 응답 수명 주기
- `cwist_http_response *cwist_http_response_create(void)`
- `void cwist_http_response_destroy(cwist_http_response *res)`
- `cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res)`

### 헤더 도우미
- `cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value)`
- `char *cwist_http_header_get(cwist_http_header_node *head, const char *key)`
- `void cwist_http_header_free_all(cwist_http_header_node *head)`

### 메서드 도우미
- `const char *cwist_http_method_to_string(cwist_http_method_t method)`
- `cwist_http_method_t cwist_http_string_to_method(const char *method_str)`

### 소켓 도우미
- `int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog)`
- `cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd))`
- `cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int))`

## 세션 매니저

### 아레나
- `void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity)`
- `void *session_arena_alloc(struct session_arena *arena, size_t size)`
- `void session_arena_reset(struct session_arena *arena)`

### 공유 세션 (침습적 참조 카운트)
- `void session_rc_init(struct session_rc_header *header, void (*destructor)(void *))`
- `void *session_shared_alloc(size_t payload_size, void (*destructor)(void *))`
- `void session_shared_inc(void *payload)`
- `void session_shared_dec(void *payload)`

### 매니저
- `void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity)`
- `void session_manager_reset(struct session_manager *manager)`
