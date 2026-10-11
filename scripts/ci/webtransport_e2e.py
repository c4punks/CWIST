#!/usr/bin/env python3
"""End-to-end WebTransport check against tests/webtransport_echo_server.

Starts the echo server, then drives it with aioquic (pip install aioquic):

  1. a session on /wt is accepted (200, no FIN on the CONNECT stream);
  2. a client bidi stream is echoed back and finished;
  3. a datagram is echoed back;
  4. a session on /nope is refused with 403;
  5. the "close" datagram makes the server end the session with a
     CLOSE_WEBTRANSPORT_SESSION capsule (code 7, reason "bye") and FIN.

Usage: webtransport_e2e.py <server-binary> <cert> <key> [port]
"""
import asyncio
import os
import ssl
import subprocess
import sys

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, FrameType, H3Connection
from aioquic.h3.events import (
    DatagramReceived,
    DataReceived,
    HeadersReceived,
    WebTransportStreamDataReceived,
)
from aioquic.quic.configuration import QuicConfiguration

TIMEOUT = 10.0


class Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.h3 = H3Connection(self._quic, enable_webtransport=True)
        self.events = asyncio.Queue()

    def quic_event_received(self, event):
        for h3_event in self.h3.handle_event(event):
            self.events.put_nowait(h3_event)

    async def next_event(self, kind, match=lambda e: True):
        while True:
            event = await asyncio.wait_for(self.events.get(), TIMEOUT)
            if isinstance(event, kind) and match(event):
                return event

    def open_session(self, path):
        session_id = self._quic.get_next_available_stream_id()
        self.h3.send_headers(
            session_id,
            [
                (b":method", b"CONNECT"),
                (b":protocol", b"webtransport"),
                (b":scheme", b"https"),
                (b":authority", b"localhost"),
                (b":path", path.encode()),
                (b"sec-webtransport-http3-draft02", b"1"),
                (b"origin", b"https://localhost"),
            ],
        )
        self.transmit()
        return session_id


def status_of(headers_event):
    return dict(headers_event.headers).get(b":status")


def parse_varint(buf, off):
    n = 1 << (buf[off] >> 6)
    v = buf[off] & 0x3F
    for b in buf[off + 1 : off + n]:
        v = (v << 8) | b
    return v, off + n


async def run(port):
    config = QuicConfiguration(
        is_client=True, alpn_protocols=H3_ALPN, max_datagram_frame_size=65536
    )
    config.verify_mode = ssl.CERT_NONE
    async with connect(
        "127.0.0.1", port, configuration=config, create_protocol=Client
    ) as client:
        session = client.open_session("/wt")
        headers = await client.next_event(
            HeadersReceived, lambda e: e.stream_id == session
        )
        assert status_of(headers) == b"200", headers.headers
        assert not headers.stream_ended, "the session's CONNECT stream was finished"
        print("[PASS] session accepted")

        stream = client.h3.create_webtransport_stream(session)
        # aioquic only recognizes a WebTransport bidi stream by the 0x41
        # signal its *peer* sends, but the signal goes one way: the server
        # answers on this stream with raw bytes. Mark it by hand.
        with client.h3._get_or_create_stream(stream) as h3_stream:
            h3_stream.frame_type = FrameType.WEBTRANSPORT_STREAM
            h3_stream.session_id = session
        client._quic.send_stream_data(stream, b"hello webtransport", end_stream=True)
        client.transmit()
        echoed = b""
        while True:
            event = await client.next_event(
                WebTransportStreamDataReceived, lambda e: e.stream_id == stream
            )
            echoed += event.data
            if event.stream_ended:
                break
        assert echoed == b"hello webtransport", echoed
        print("[PASS] bidi stream echoed and finished")

        client.h3.send_datagram(session, b"ping-dgram")
        client.transmit()
        dgram = await client.next_event(DatagramReceived)
        assert dgram.stream_id == session and dgram.data == b"ping-dgram", dgram
        print("[PASS] datagram echoed")

        refused = client.open_session("/nope")
        headers = await client.next_event(
            HeadersReceived, lambda e: e.stream_id == refused
        )
        assert status_of(headers) == b"403", headers.headers
        print("[PASS] session refused with 403")

        client.h3.send_datagram(session, b"close")
        client.transmit()
        capsule = b""
        while True:
            event = await client.next_event(
                DataReceived, lambda e: e.stream_id == session
            )
            capsule += event.data
            if event.stream_ended:
                break
        ctype, off = parse_varint(capsule, 0)
        clen, off = parse_varint(capsule, off)
        body = capsule[off : off + clen]
        assert ctype == 0x2843, hex(ctype)
        assert int.from_bytes(body[:4], "big") == 7 and body[4:] == b"bye", body
        print("[PASS] server closed the session (code 7, reason 'bye')")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    server, cert, key = sys.argv[1:4]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 19443
    proc = subprocess.Popen(
        [server, str(port), cert, key], stdout=subprocess.PIPE, text=True
    )
    try:
        line = proc.stdout.readline()
        if line.strip() != "READY":
            print("server did not start:", line, file=sys.stderr)
            return 1
        asyncio.run(asyncio.wait_for(run(port), 4 * TIMEOUT))
        if proc.poll() is not None:
            print("server exited early:", proc.returncode, file=sys.stderr)
            return 1
        print("All WebTransport end-to-end checks passed")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
