/**
 * The WebTransport client API is declared in every build (#338): a build
 * without the client role (CWIST_WEBTRANSPORT=server or none, and main,
 * whose lsquic has no lsquic_wt.h) must still define every function, as a
 * stub that fails. Linking this test proves the symbols exist; the calls
 * check that bad arguments are refused in every configuration.
 */
#include <cwist/net/http/http3_client.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, msg)                        \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            failures++;                         \
        }                                       \
    } while (0)

int main(void) {
    cwist_webtransport_client_session *session = (cwist_webtransport_client_session *)0;
    cwist_error_t err = cwist_http3_client_webtransport_connect(NULL, "/wt", NULL, &session);
    CHECK(err.error.err_i16 != 0, "connect without a client fails");
    CHECK(session == NULL, "connect without a client leaves no session");

    CHECK(cwist_webtransport_client_poll(NULL, 0) == -1, "poll without a client fails");
    CHECK(!cwist_webtransport_client_is_open(NULL), "no session is not open");
    CHECK(cwist_webtransport_client_open_bidi(NULL) == NULL, "open_bidi without a session fails");
    CHECK(cwist_webtransport_client_open_uni(NULL) == NULL, "open_uni without a session fails");

    char buf[4];
    CHECK(cwist_webtransport_client_stream_read(NULL, buf, sizeof(buf)) == -1,
          "read without a stream fails");
    CHECK(cwist_webtransport_client_stream_write(NULL, "x", 1) == -1,
          "write without a stream fails");
    CHECK(cwist_webtransport_client_stream_flush(NULL) == -1, "flush without a stream fails");
    CHECK(cwist_webtransport_client_stream_close(NULL) == -1, "close without a stream fails");
    CHECK(cwist_webtransport_client_send_datagram(NULL, "x", 1) == -1,
          "datagram without a session fails");
    CHECK(cwist_webtransport_client_close(NULL, 0, NULL) == -1,
          "closing no session fails");

    if (failures) return 1;
    printf("WebTransport client API: every symbol links and refuses bad arguments\n");
    return 0;
}
