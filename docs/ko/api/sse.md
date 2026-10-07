# Server-Sent Events API

*헤더:* `<cwist/net/http/sse.h>`

`text/event-stream` 데이터를 보내기 위한 도우미입니다. 사용 방법은 두 가지입니다.

* **응답 모드** (`cwist_sse_response_*`): 일반 핸들러의 응답 본문에 프레임을 덧붙입니다.
  응답은 핸들러가 반환할 때 전송되므로 정해진 묶음의 이벤트에 알맞습니다.
* **스트림 모드** (`cwist_sse_stream_*`): 헤더를 클라이언트 소켓에 즉시 쓰고, 각 이벤트를
  쓰는 즉시 보냅니다. 시간에 따라 만들어지는 이벤트에 알맞습니다.

대부분의 함수는 `cwist_error_t`를 반환합니다. 잘못된 인자(`NULL` 포인터나 `-1`보다 작은
`retry_ms`)로 거부되거나 프레임을 만들지 못한 호출은 `err_i16 == -1`을 반환합니다. 테스트는
`err.error.err_i16 == 0`으로 성공을 확인합니다.

모든 이벤트는 `field:value` 형식의 줄들과 그 뒤의 빈 줄로 기록됩니다. 필드 순서는 `id`,
`event`, `retry`, `data`입니다. `NULL`인 `event`나 `id`는 생략합니다. 줄바꿈이 들어 있는
`data` 문자열은 줄마다 `data:` 줄 하나로 나뉘고, `NULL` `data`는 빈 `data:` 줄을 만듭니다.
콜론 뒤에 공백이 없으므로 이벤트 `("update", "42", 1500, "first\nsecond")`는 다음과 같이
전송됩니다.

```
id:42
event:update
retry:1500
data:first
data:second

```

## 타입

### `cwist_sse_event_t`
```c
typedef struct cwist_sse_event {
    const char *event;
    const char *id;
    int retry_ms;
    const char *data;
} cwist_sse_event_t;
```
`cwist_sse_response_write`와 `cwist_sse_stream_write`에 쓰는 구조화된 이벤트입니다.
`NULL` 필드는 생략합니다. `retry_ms`는 `0` 이상이면 전송되고 `-1`이면 생략됩니다.

### `cwist_sse_stream_t`
`cwist_sse_stream_open`으로 만드는, 열린 스트림에 대한 불투명 핸들입니다.

## 응답 모드

### `cwist_sse_response_init`
```c
cwist_error_t cwist_sse_response_init(cwist_http_response *res);
```
`res`에 SSE 헤더를 추가하고 `res->keep_alive`를 `true`로 설정합니다. 헤더는 다음과 같습니다.

| 헤더 | 값 |
|--------|-------|
| `Content-Type` | `text/event-stream; charset=utf-8` |
| `Cache-Control` | `no-cache` |
| `X-Accel-Buffering` | `no` |

`res`가 `NULL`이거나 헤더를 추가하지 못하면 `err_i16 == -1`을 반환합니다.

### `cwist_sse_response_event`
```c
cwist_error_t cwist_sse_response_event(cwist_http_response *res, const char *event,
                                       const char *id, int retry_ms, const char *data);
```
응답 본문에 이벤트 하나를 덧붙입니다. 해당 필드를 생략하려면 `event`나 `id`에 `NULL`을,
`retry_ms`에 `-1`을 넘기세요. `-1`보다 작은 `retry_ms`는 `err_i16 == -1`로 거부됩니다.

### `cwist_sse_response_write`
```c
cwist_error_t cwist_sse_response_write(cwist_http_response *res,
                                       const cwist_sse_event_t *event);
```
`cwist_sse_response_event`와 같지만 필드를 `cwist_sse_event_t`에서 가져옵니다. `event`가
`NULL`이면 `err_i16 == -1`을 반환합니다.

### `cwist_sse_response_comment`
```c
cwist_error_t cwist_sse_response_comment(cwist_http_response *res, const char *comment);
```
주석 줄(`:text`)과 빈 줄을 덧붙입니다. 클라이언트는 주석을 무시하므로, 연결을 열어 두는
하트비트로 쓰입니다.

## 스트림 모드

### `cwist_sse_stream_open`
```c
cwist_sse_stream_t *cwist_sse_stream_open(cwist_http_request *req);
```
`200 OK` 상태 줄과 SSE 헤더를 `req->client_fd`에 쓰고 `req->upgraded`를 설정합니다.
핸들러가 반환하면 서버는 `req->upgraded`를 보고 자체 응답을 쓰지 않으며 연결을 유지하지도
않으므로, 모든 이벤트는 핸들러가 반환하기 전에 보내야 합니다. `req`가 `NULL`이거나,
`req->client_fd`가 음수이거나, `req->upgraded`가 이미 설정되어 있거나, 쓰기에 실패하면
`NULL`을 반환합니다.

### `cwist_sse_stream_send`
```c
cwist_error_t cwist_sse_stream_send(cwist_sse_stream_t *stream, const char *event,
                                    const char *id, int retry_ms, const char *data);
```
`cwist_sse_response_event`와 같은 필드 규칙으로 스트림에 이벤트 하나를 보냅니다. 쓰기는
뮤텍스로 직렬화되므로 하나의 스트림을 여러 스레드에서 사용할 수 있습니다. 쓰기가 실패하면
스트림은 닫힘으로 표시되고, 이후의 모든 전송은 `err_i16 == -1`을 반환합니다.

### `cwist_sse_stream_write`
```c
cwist_error_t cwist_sse_stream_write(cwist_sse_stream_t *stream,
                                     const cwist_sse_event_t *event);
```
`cwist_sse_stream_send`와 같지만 필드를 `cwist_sse_event_t`에서 가져옵니다.

### `cwist_sse_stream_comment`
```c
cwist_error_t cwist_sse_stream_comment(cwist_sse_stream_t *stream, const char *comment);
```
스트림에 주석 줄을 보냅니다. 예를 들어 하트비트로 씁니다.

### `cwist_sse_stream_close`
```c
void cwist_sse_stream_close(cwist_sse_stream_t *stream);
```
스트림 핸들을 해제합니다. 클라이언트 소켓은 닫지 않습니다. `NULL`을 넘겨도 됩니다.

## 편의 매크로

| 매크로 | 전개 결과 |
|-------|-----------|
| `CWIST_SSE_EVENT(payload)` | `data`만 설정된 `cwist_sse_event_t` |
| `CWIST_SSE_NAMED(name, payload)` | `event`와 `data`가 설정된 `cwist_sse_event_t` |
| `CWIST_SSE_JSON(payload)` | `CWIST_SSE_NAMED("message", payload)` |
| `CWIST_SSE_RESPONSE_SEND(res, event)` | `cwist_sse_response_write(res, &event)` |
| `CWIST_SSE_STREAM_SEND(stream, event)` | `cwist_sse_stream_write(stream, &event)` |

두 `SEND` 매크로는 이벤트의 주소를 취하므로 lvalue로 받습니다.

## 예제

정해진 묶음의 이벤트를 보내는 핸들러입니다
([튜토리얼 08](../../../tutorials/08-sse-realtime/README.md)에서 쓰는 패턴입니다).

```c
static void handle_events(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sse_response_init(res);
    cwist_sse_response_event(res, "message", "1", -1, "Welcome to Live SSE Stream");
    cwist_sse_response_event(res, "update", "2", -1, "Tick 1");
}
```

이벤트를 소켓으로 직접 스트리밍하는 핸들러입니다. 이벤트는 핸들러가 실행되는 동안 전송됩니다.

```c
static void handle_stream(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    cwist_sse_stream_t *stream = cwist_sse_stream_open(req);
    if (!stream) return;

    cwist_sse_event_t ev = CWIST_SSE_NAMED("tick", "1");
    CWIST_SSE_STREAM_SEND(stream, ev);
    cwist_sse_stream_comment(stream, "heartbeat");

    cwist_sse_stream_close(stream);
}
```

각 호출의 정확한 출력은 `tests/test_sse.c`에서 확인할 수 있습니다.
