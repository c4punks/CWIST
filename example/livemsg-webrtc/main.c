#include <cwist/livemsg.h>

static int process(cwist_livemsg_peer *peer, const void *msg, size_t len, void **result,
                   void *user_data) {
    (void)peer;
    (void)len;
    (void)user_data;
    *result = (void *)msg;
    return CWIST_LIVEMSG_OK;
}

static int respond(cwist_livemsg_peer *peer, void *result, cwist_livemsg_reply *reply,
                   void *user_data) {
    (void)peer;
    (void)user_data;
    return cwist_livemsg_reply_set_json(reply, result);
}

static cwist_livemsg_config make_config(void) {
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WEBRTC);
    config.port = 8080;
    config.udp_port = 9000;
    config.signaling_path = "/signal";
    config.codec = CWIST_LIVEMSG_CODEC_JSON;
    config.process = process;
    config.respond = respond;
    return config;
}

int main(void) {
    return cwist_livemsg_serve(make_config) == 0 ? 0 : 1;
}
