# CWIST WebRTC DataChannel example

DataChannel-only WebRTC echo server: the browser sends an SDP offer over
HTTP POST, the CWIST server (acting as an ICE-lite endpoint) answers, and
both ends run ICE + DTLS + SCTP (RFC 8831) so messages on a DataChannel are
echoed back.

## Build

```sh
make -C ../.. libcwist.a        # builds lib/usrsctp too (CWIST_WEBRTC=1 default)
make
```

## Run

```sh
./webrtc-server
# signaling: http://localhost:8080/ (webrtc UDP port <ephemeral>)
```

Open http://localhost:8080/ in a browser, click **Connect**, type a message
and press **Send**. Every DataChannel message is echoed back and printed on
the server console.

## Layout

- `main.c` — cwist app serving `index.html` (GET /) and SDP answers
  (POST /application/sdp), plus the echo logic on the cwist webrtc ctx.
- `index.html` — minimal RTCPeerConnection client using a DataChannel.

## Notes / limitations (MVP)

- The server is ICE-lite: it answers STUN binding requests and treats a
  valid `USE-CANDIDATE` request as the nominated pair. The browser must be
  the controlling agent (the default).
- The browser connects to the host candidate advertised in the answer
  (`a=candidate:` with the server's best-guess local IPv4). Set the env
  var or edit the code behind a NAT; there is no STUN/TURN server support.
- Ordered delivery only; the answer pins `max-message-size:262144`.
