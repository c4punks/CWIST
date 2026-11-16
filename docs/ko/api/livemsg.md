# 라이브 메시지 런타임(실험적)

`cwist_livemsg`는 WebSocket 또는 WebRTC DataChannel 위에서 완성된 메시지 단위의
애플리케이션 콜백을 실행합니다. 전송, 코덱, 프레이밍, 선택적 SDP 시그널링,
논블로킹 송신은 런타임이 담당하고 애플리케이션은 `process()`와 `respond()`만
제공합니다.

*헤더:* `<cwist/livemsg.h>`

이 API는 v4.x에서 실험적이며 v4.0 API 보장 범위를 변경하지 않습니다.

## 최소 서버

```c
#include <cwist/livemsg.h>

static int process(cwist_livemsg_peer *peer, const void *msg, size_t len,
                   void **result, void *user_data) {
    (void)peer; (void)len; (void)user_data;
    *result = (void *)msg;
    return CWIST_LIVEMSG_OK;
}

static int respond(cwist_livemsg_peer *peer, void *result,
                   cwist_livemsg_reply *reply, void *user_data) {
    (void)peer; (void)user_data;
    return cwist_livemsg_reply_set_json(reply, result);
}

static cwist_livemsg_config make_config(void) {
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WS);
    config.port = 8080;
    config.codec = CWIST_LIVEMSG_CODEC_JSON;
    config.process = process;
    config.respond = respond;
    return config;
}

int main(void) {
    return cwist_livemsg_serve(make_config);
}
```

`example/livemsg-websocket/livemsg-websocket`을 빌드해 실행한 뒤 WebSocket
클이언트를 `ws://localhost:8080/`에 연결하면 JSON 메시지를 해석해 그대로
응답합니다. `example/livemsg-webrtc/livemsg-webrtc`는 같은 콜백을 사용하고
`POST http://localhost:8080/signal`에서 SDP를 받으며, ICE/DTLS/SCTP에는 UDP
9000번을 사용합니다. 시그널링 요청과 응답 본문의 형식은 `application/sdp`입니다.

## 설정

콜백과 포트를 지정하기 전에 `cwist_livemsg_config_init()`을 호출합니다.
`ws_path`는 `/`로, WebRTC의 `signaling_path`는 `/signal`로 초기화됩니다.
내장 HTTP 엔드포인트를 끄려면 `signaling_path`를 `NULL`로 설정합니다.
`port`와 `udp_port`의 초기값은 0이므로 애플리케이션에서 명시하는 것이 좋습니다.
WebRTC UDP 포트를 임의 할당하려는 경우는 예외입니다.

`transport`는 향후 확장을 위한 비트마스크이지만 v1은 다음 중 정확히 하나만
허용합니다.

- `CWIST_LIVEMSG_TRANSPORT_WS`: C1M 리액터의 논블로킹 WebSocket.
- `CWIST_LIVEMSG_TRANSPORT_WEBRTC`: DataChannel마다 피어 하나를 만들고,
  ICE/DTLS/SCTP 상태를 한 프로세스가 소유.

두 전송을 함께 선택하거나 알 수 없는 전송을 선택하면 serve 래퍼가 `-ENOTSUP`을
반환합니다. `create()`는 `NULL`을 반환하고 `errno`를 설정합니다. 두 콜백은
필수입니다. 내장 코덱은 `RAW`와 `JSON`입니다. Protobuf와 FlatBuffers는 `RAW`와
애플리케이션 직렬화 코드를 사용하며, Protobuf에는 `cwist_pb_*`를 활용할 수
있습니다. 룸과 동시 전송 서빙은 v1 범위가 아닙니다.

`cwist_livemsg_run()`은 항상 C1M 워커 프로세스 하나를 사용합니다.
`CWIST_WORKERS`를 상속하지 않으며, fork된 워커끼리 WebRTC ICE/DTLS 상태를
공유할 수 없습니다.

## 메시지와 결과 소유권

수신 메시지마다 약 512 KiB 아레나를 만들고 원시 바이트를 복사한 뒤 `process()`와
`respond()` 동안 유지합니다. 콜백 순서가 끝나면 아레나와 응답 버퍼를 즉시
해제하므로 애플리케이션 해제 콜백은 없습니다.

- `RAW`에서 `msg`는 복사된 바이트이고 `len`은 정확한 길이입니다.
- `JSON`에서 `msg`는 빌려온 `cJSON *`이며 `len`은 원시 길이입니다.
- 독립 저장소가 필요한 결과는 `cwist_livemsg_alloc(peer, size)`로 할당하거나
  디코딩된 입력의 안쪽 데이터를 가리켜야 합니다.
- 디코딩된 JSON 트리는 `respond()` 후 런타임이 삭제합니다.
- `cwist_livemsg_reply_set_json()`은 빌려온 트리를 직렬화할 뿐 삭제하지
  않습니다. 별도로 만든 cJSON 트리의 소유권은 애플리케이션에 남습니다.
- 입력, 결과, 아레나 할당, 응답 핸들을 콜백 밖에서 보관하면 안 됩니다.

어느 콜백에서든 `CWIST_LIVEMSG_NO_REPLY`를 반환하거나, 빈 응답 버퍼로 성공을
반환하면 패킷을 전송하지 않습니다. 다른 0이 아닌 콜백 결과도 응답을 억제하며
세션은 후속 메시지에 계속 사용할 수 있습니다.

## 응답, 푸시, 백프레셔

`cwist_livemsg_reply_write()`는 불투명 응답 버퍼를 동적으로 확장합니다.
JSON 응답은 텍스트 메시지, RAW 응답은 바이너리 메시지로 전송됩니다.

`cwist_livemsg_send_raw()`와 `cwist_livemsg_send_json()`은 서버 주도 푸시에도
사용합니다. `cwist_livemsg_peer_ctx()`와 `cwist_livemsg_foreach_peer()`를 함께
사용해 모든 피어에 브로드캐스트할 수 있습니다. 피어 포인터는
`on_disconnect()`가 반환되기 전까지만 유효하므로 보관하면 안 됩니다. 피어를
사용하거나 목록을 순회하는 호출은 해당 전송의 리액터 스레드에서 실행해야
합니다. WebRTC의 저수준 송신 큐는 스레드 안전하지만, v1 피어 수명 규약은
리액터 범위로 제한합니다.

두 전송에 같은 제한이 적용됩니다.

- 송수신 메시지 최대 256 KiB. API 호출이 초과하면 `-EMSGSIZE`를 반환합니다.
  WebSocket은 크기 초과 입력을 1009로, 잘못된 JSON을 1007로 종료합니다.
- WebSocket 피어당 송신 큐 최대 4 MiB. 한 WebRTC 연결의 DataChannel들은
  해당 연결의 4 MiB 큐를 공유합니다. 큐가 가득 차면 리액터를 멈추지 않고
  즉시 `-EAGAIN`을 반환합니다.

인자와 상태 오류에는 `-EINVAL` 또는 `-ENOTSUP`, 할당과 전송 오류에는
`-ENOMEM` 또는 `-EIO`를 사용합니다.

## 외부 시그널링과 임베딩

내장 시그널링에는 한 줄 serve 함수로 충분합니다. 다른 경로에서 SDP를 교환하는
애플리케이션은 수명 주기 API를 사용할 수 있습니다.

```c
cwist_livemsg_config config;
cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WEBRTC);
config.signaling_path = NULL;
config.udp_port = 9000;
config.process = process;
config.respond = respond;

cwist_livemsg_ctx *ctx = cwist_livemsg_create(&config);
char *answer = NULL;
if (ctx && cwist_livemsg_webrtc_offer(ctx, offer_sdp, &answer) == 0) {
    /* answer를 애플리케이션의 시그널링 채널로 반환합니다. */
    cwist_free(answer);
}
/* 필요하면 이 프로세스에서 cwist_livemsg_run(ctx)으로 HTTP 포트를 엽니다. */
cwist_livemsg_destroy(ctx);
```

`answer_sdp_out`은 `cwist_alloc()`으로 할당되므로 `cwist_free()`로 해제해야
합니다. 컨텍스트를 만들면 WebRTC 리액터가 시작되므로 `cwist_livemsg_run()` 전에도
Offer를 처리할 수 있습니다. 실행 루프가 반환되고 외부 Offer 제출이 모두 끝난
후에만 컨텍스트를 해제하세요.

## 검증

`make test_livemsg`는 실제 WebSocket 서버와 동시 클라이언트를 사용해 RAW/JSON
에코, 무응답, 아레나 결과, 푸시, 브로드캐스트, 메시지 제한, 송신 큐 포화를
검증합니다. `make test_livemsg_webrtc`는 외부 SDP 시그널링과 실제
ICE/DTLS/SCTP 루프백 채널을 열어 푸시, 200000바이트 에코, 종료 정리를 확인합니다.
두 테스트는 프로젝트의 새니타이저 구성에서도 실행됩니다.
