#ifndef CWIST_HTTP2_FLOW_CONTROL_INTERNAL_H
#define CWIST_HTTP2_FLOW_CONTROL_INTERNAL_H

#include <cwist/net/http/http2_flow_control.h>

void cwist_http2_stream_flow_control_adjust_send_window(
    cwist_http2_stream_flow_control *flow_control, int32_t delta);

#endif
