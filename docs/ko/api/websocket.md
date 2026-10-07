# WebSocket API

WebSocket 모듈은 일반 HTTP 연결을 지속적인 WebSocket 연결로 업그레이드할 수 있게 해 줍니다.

## 함수

### `cwist_websocket_upgrade`
들어온 HTTP 요청을 WebSocket 연결로 업그레이드합니다.
```c
cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd);
```
- 성공하면 `cwist_websocket` 컨텍스트를, 실패하면 `NULL`을 반환합니다.
- 핸드셰이크를 자동으로 처리하고 `101 Switching Protocols` 응답을 보냅니다.
- **소유권:** 반환된 포인터는 호출자가 소유하며 `cwist_websocket_destroy`로 파괴해야 합니다.

### `cwist_websocket_receive`
WebSocket 프레임 하나를 받습니다.
```c
cwist_ws_frame *cwist_websocket_receive(cwist_websocket *ws);
```
- 프레임이 도착할 때까지 블로킹합니다.
- 연결이 닫혔거나 에러가 나면 `NULL`을 반환합니다.
- **소유권:** 반환된 프레임은 `cwist_websocket_frame_destroy`로 해제해야 합니다.

### `cwist_websocket_send`
데이터를 WebSocket 프레임으로 보냅니다.
```c
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len);
```

## Opcode 종류
- `CWIST_WS_FRAME_TEXT`: 텍스트 데이터 (UTF-8)
- `CWIST_WS_FRAME_BINARY`: 바이너리 데이터
- `CWIST_WS_FRAME_CLOSE`: 연결 종료
- `CWIST_WS_FRAME_PING` / `CWIST_WS_FRAME_PONG`: 하트비트

## `cwist_app`과의 연동
WebSocket 라우트는 `cwist_app_ws`로 등록합니다.

```c
void my_ws_handler(cwist_websocket *ws) {
    // Handling logic
}

cwist_app_ws(app, "/chat", my_ws_handler);
```

## 비동기 (C1M) WebSocket API

*헤더:* `<cwist/net/websocket/websocket_async.h>`

C1M 모드(`CWIST_C1M_MODE=1`)에서는 리액터가 소켓을 감시하다가 `cwist_websocket_receive`에서
블로킹하는 대신 완성된 메시지를 콜백으로 전달합니다. 이 경로의 라우트는 `cwist_app_ws_async`로
등록합니다.

### `cwist_app_ws_async`
```c
void cwist_app_ws_async(cwist_app *app, const char *path,
                        cwist_ws_on_message_t on_message, void *user_data);
```
리액터가 구동하는 WebSocket 라우트를 등록합니다. 완성된 메시지마다 리액터 스레드에서
`on_message(ws, frame, user_data)`를 호출합니다. `user_data`는 그대로 전달되는 불투명
포인터로, 핸들러별 상태(브로드캐스트 목록, 방 컨텍스트 등)에 사용합니다.

### `cwist_ws_on_message_t`
```c
typedef void (*cwist_ws_on_message_t)(cwist_websocket_async *ws,
                                      cwist_ws_frame *frame,
                                      void *user_data);
```
완성된 메시지마다 한 번 호출되는 콜백입니다. 콜백은 **반드시** `cwist_websocket_frame_destroy()`로
`frame`을 해제해야 합니다. 이 콜백 안에서 `cwist_websocket_async_send()`와
`cwist_websocket_async_close()`를 호출해도 안전합니다.

### `cwist_websocket_async_send`
```c
int cwist_websocket_async_send(cwist_websocket_async *ws,
                               cwist_ws_opcode_t opcode,
                               const uint8_t *data, size_t len);
```
FIN으로 끝나는 프레임 하나를 블로킹 없이 보냅니다. `EAGAIN`이나 부분 쓰기가 발생하면 나머지를
리액터의 단발성 쓰기 슬롯에 맡겨 두었다가 소켓이 쓰기 가능해지면 비웁니다. 성공하면 `0`,
실패하면 `-1`을 반환합니다.

### `cwist_websocket_async_close`
```c
void cwist_websocket_async_close(cwist_websocket_async *ws);
```
블로킹 없이 종료 핸드셰이크를 시작합니다. CLOSE 프레임을 큐에 넣고 연결 정리를 예약합니다.
`on_message` 콜백 안에서 호출해도 안전하며, `NULL`은 무시합니다.

## GraphQL 구독 (graphql-ws)

> v3.8부터 지원합니다. 이 기능은 명시적으로 켰을 때만 동작하며, 사용하지 않으면 기존 동작을
> 바꾸지 않습니다.

`cwist/graphql_ws.h`는 논블로킹 WebSocket 전송 위에서 `graphql-transport-ws` 서브 프로토콜을
구현합니다.

- `connection_init` -> `connection_ack`. init 전에 `subscribe`를 보내면 코드 `4429`로 소켓을
  닫습니다.
- `subscribe {id, payload:{query,variables,operationName}}` -> 게시된 이벤트마다
  `next {id, payload:{data:{...}}}` 하나, 연산을 시작하지 못하면 `error {id, payload:[...]}`,
  끝나면 `complete {id}`.
- init 이후 `ping`/`pong` JSON keepalive. 잘못된 트래픽은 `4400`/`4401`로 닫고, 중복 연산 id는
  `4409`로 닫습니다.

구독 필드는 `cwist_graphql_ws_add_subscription()`으로 엔드포인트에 등록하며, 리졸버는 이벤트
소스(`cwist_graphql_event_source_create()` + `cwist_graphql_event_source_add_topic()`)를
반환합니다. 애플리케이션 코드는 프로세스 내 스레드 안전 토픽 브로커
`cwist_graphql_publish(topic, payload)`로 이벤트를 퍼뜨립니다. 실제 전송은 소유한 리액터
스레드에서 일어나므로 publish는 어느 스레드에서나 호출할 수 있습니다.

```c
cwist_graphql_ws_t *gws = cwist_graphql_ws_create(schema);
cwist_graphql_ws_add_subscription(gws, "counter", counter_subscribe, NULL);
cwist_graphql_ws_attach(gws, upgraded_fd, reactor, NULL, 0);
/* elsewhere, any thread: */
cwist_graphql_publish("counter", payload); /* -> next {data:{counter: payload}} */
```

클라이언트의 `complete {id}`와 소켓 종료 모두 누수 없이 구독을 정리합니다. 소켓 쌍으로 구동하는
리액터 위의 종단 간 예제는 `tests/test_graphql_subscriptions.c`를 보세요.
