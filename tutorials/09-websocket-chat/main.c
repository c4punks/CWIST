#include <cwist/app.h>
#include <cwist/net/websocket/websocket.h>

/* Called once per connection after the upgrade; the connection is closed and
 * freed when this returns, so keep receiving until the client goes away. */
static void ws_on_message(cwist_websocket *ws) {
    cwist_ws_frame *frame;
    while ((frame = cwist_websocket_receive(ws)) != NULL) {
        if (frame->opcode == CWIST_WS_FRAME_CLOSE) {
            cwist_websocket_frame_destroy(frame);
            break;
        }
        if (frame->opcode == CWIST_WS_FRAME_TEXT) {
            cwist_websocket_send(ws, CWIST_WS_FRAME_TEXT, (const uint8_t *)"Echo: ", 6);
            cwist_websocket_send(ws, CWIST_WS_FRAME_TEXT, frame->payload, frame->payload_len);
        }
        cwist_websocket_frame_destroy(frame);
    }
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_ws(app, "/ws", ws_on_message);
    cwist_app_listen(app, 8088);
    cwist_app_destroy(app);
    return 0;
}
