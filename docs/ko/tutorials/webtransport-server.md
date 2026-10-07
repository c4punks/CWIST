# CWIST로 WebTransport 서버 만들기

CWIST는 HTTP/3 서버 위에서 WebTransport를 제공합니다. WebTransport에는 TLS와 QUIC가 필요하며,
개발 중에도 브라우저가 신뢰하는 인증서가 필요합니다.

> **플랫폼 참고:** HTTP/3 리스너는 Linux에서 `epoll`을, macOS/BSD에서 이식 가능한 polling 경로를
> 사용합니다. ECN 수신 메타데이터는 선택 사항이므로, BSD 소켓 확장이 없어도 WebTransport 빌드나
> 리스너 시작이 막히지 않습니다.

## 1. HTTP/3과 세션 핸들러 설정하기

```c
#include <cwist/sys/app/app.h>

static void on_webtransport(cwist_http_request *req,
                            cwist_http_response *res,
                            void *session) {
    (void)req;
    (void)session;
    res->status_code = CWIST_HTTP_OK; /* Accept the CONNECT session. */
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_https3(app, true);       /* Or configure certificate/key first. */
    cwist_app_use_webtransport(app, on_webtransport);
    int rc = cwist_app_listen(app, 4433);
    cwist_app_destroy(app);
    return rc;
}
```

`cwist_app_use_http3(app, true)`는 임시 인증서를 만들기 때문에 로컬 실험에 알맞습니다. 브라우저
배포에는 실제 TLS 인증서를 쓰고, 공개 엔드포인트가 UDP로 도달 가능하게 하세요.

## 2. 세션 프로토콜 설계하기

WebTransport는 애플리케이션에 신뢰할 수 있는 스트림과 선택적인 데이터그램을 제공합니다. 각
애플리케이션 메시지를 버전이 있는 작은 봉투로 감싸고, 메시지 크기에 상한을 두고, 명령을 받기 전에
세션을 인증하세요. CRUD, 채팅 기록, 확인 응답에는 스트림을 쓰고, 데이터그램은 커서 위치나 게임
스냅숏처럼 대체 가능한 상태에만 쓰세요.

데이터그램이 도착한다고, 한 번만 도착한다고, 순서대로 도착한다고 가정하지 마세요. 단조 증가하는
순번을 넣고 오래된 갱신은 버리세요. 신뢰할 수 있는 스트림 데이터의 경우, 프로토콜이 의도적으로 큰
메시지를 나눌 때 CWIST의 순번 도우미로 애플리케이션 수준의 조각난 페이로드를 다시 조립할 수
있습니다.

## 3. 안전하게 운영하기

동시 스트림, 버퍼에 쌓인 바이트, 유휴 시간에 세션별 한도를 두세요. 세션을 할당하기 전에
지원하지 않는 CONNECT 경로를 거부하세요. 요청 식별자, 협상된 출처, 종료 이유, 전송 에러는
기록하되, 자격 증명이나 애플리케이션 페이로드는 절대 기록하지 마세요. 공개 리스너에서 켜기 전에
WebTransport 회귀 테스트로 핸들러를 실험해 보세요.
