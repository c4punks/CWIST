#!/usr/bin/env python3
"""Test HTTP/3 requests while a UDP proxy changes the client address.

Requires Python 3.11 or later and aioquic==1.4.0. No root access is necessary.
On macOS, use --migration-host with a second configured local IPv4 address.
"""

import argparse
import asyncio
import os
import ssl
import sys

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated

TIMEOUT = 10
STREAMS_PER_ROUND = 3


class Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.h3 = H3Connection(self._quic)
        self.responses = {}
        self.headers = {}
        self.bodies = {}
        self.peer_cid_ready = asyncio.Event()

    def datagram_received(self, data, addr):
        super().datagram_received(data, addr)
        if self._quic._peer_cid_available:
            self.peer_cid_ready.set()

    def quic_event_received(self, event):
        if isinstance(event, ConnectionTerminated):
            for response in self.responses.values():
                if not response.done():
                    response.set_exception(RuntimeError(str(event)))
        for item in self.h3.handle_event(event):
            if isinstance(item, HeadersReceived):
                self.headers[item.stream_id] = dict(item.headers)
            elif isinstance(item, DataReceived):
                self.bodies[item.stream_id].extend(item.data)
            if isinstance(item, (HeadersReceived, DataReceived)) and item.stream_ended:
                response = self.responses[item.stream_id]
                if not response.done():
                    response.set_result(
                        (
                            self.headers[item.stream_id],
                            bytes(self.bodies[item.stream_id]),
                        )
                    )

    def start_request(self, prefix):
        stream = self._quic.get_next_available_stream_id()
        response = asyncio.get_running_loop().create_future()
        self.responses[stream] = response
        self.bodies[stream] = bytearray()
        self.h3.send_headers(
            stream,
            [
                (b":method", b"POST"),
                (b":scheme", b"https"),
                (b":authority", b"localhost"),
                (b":path", b"/echo"),
            ],
        )
        self.h3.send_data(stream, prefix, end_stream=False)
        self.transmit()
        return stream, response

    def finish_request(self, stream, suffix):
        self.h3.send_data(stream, suffix, end_stream=True)
        self.transmit()

    async def check_response(self, stream, expected_body, connection=None):
        response = self.responses[stream]
        headers, body = await asyncio.wait_for(response, TIMEOUT)
        del self.responses[stream], self.headers[stream], self.bodies[stream]
        assert headers[b":status"] == b"200", headers
        assert body == expected_body, "request data changed during migration"
        current = headers[b"x-test-connection"]
        if connection is not None:
            assert current == connection, "migration created another connection"
        return current

    def clear_responses(self):
        for response in self.responses.values():
            if response.done() and not response.cancelled():
                response.exception()
            else:
                response.cancel()
        self.responses.clear()
        self.headers.clear()
        self.bodies.clear()


class Upstream(asyncio.DatagramProtocol):
    def __init__(self, nat):
        self.nat = nat

    def datagram_received(self, data, addr):
        if self.nat.client is not None:
            self.nat.front.sendto(data, self.nat.client)

    def error_received(self, exc):
        self.nat.errors.append(str(exc))


class NAT(asyncio.DatagramProtocol):
    """Keep the front address fixed and change the server-side source socket."""

    def __init__(self, server_port):
        self.server = ("127.0.0.1", server_port)
        self.client = None
        self.front = None
        self.upstream = None
        self.errors = []
        self.addresses = []

    def connection_made(self, transport):
        self.front = transport

    def datagram_received(self, data, addr):
        self.client = addr
        self.upstream.sendto(data)

    async def rebind(self, host):
        transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(
            lambda: Upstream(self), local_addr=(host, 0), remote_addr=self.server
        )
        address = transport.get_extra_info("sockname")
        if self.addresses:
            assert address != self.addresses[-1], "NAT source address did not change"
        self.addresses.append(address)
        old = self.upstream
        self.upstream = transport
        if old is not None:
            old.close()

    async def start(self):
        await self.rebind("127.0.0.1")
        await asyncio.get_running_loop().create_datagram_endpoint(
            lambda: self, local_addr=("127.0.0.1", 0)
        )
        return self.front.get_extra_info("sockname")[1]

    def close(self):
        if self.front is not None:
            self.front.close()
        if self.upstream is not None:
            self.upstream.close()


async def exercise(server_port, rounds, label, migration_host):
    nat = NAT(server_port)
    try:
        port = await nat.start()
        config = QuicConfiguration(is_client=True, alpn_protocols=["h3"])
        config.verify_mode = ssl.CERT_NONE
        async with connect(
            "127.0.0.1", port, configuration=config, create_protocol=Client
        ) as client:
            try:
                # Record the server connection before the first address change.
                stream, _ = client.start_request(label.encode())
                client.finish_request(stream, b"/baseline")
                connection = await client.check_response(
                    stream, f"{label}/baseline".encode()
                )
                for turn in range(rounds):
                    requests = []
                    for index in range(STREAMS_PER_ROUND):
                        prefix = (
                            f"{label}/{turn}/{index}:".encode() + bytes(range(256)) * 16
                        )
                        suffix = os.urandom(8192)
                        stream, response = client.start_request(prefix)
                        requests.append((stream, response, prefix, suffix))
                    await asyncio.wait_for(client.ping(), TIMEOUT)
                    assert all(not response.done() for _, response, _, _ in requests), (
                        "server completed a request before FIN"
                    )
                    # Alternate port-only changes with IP address changes.
                    host = nat.addresses[-1][0]
                    if turn % 2:
                        host = migration_host if host == "127.0.0.1" else "127.0.0.1"
                    await nat.rebind(host)
                    if turn == 1:
                        # lsquic limits the rate of new Connection IDs.
                        await asyncio.wait_for(client.peer_cid_ready.wait(), TIMEOUT)
                        old_cid = client._quic._peer_cid.cid
                        client.change_connection_id()
                        assert client._quic._peer_cid.cid != old_cid, (
                            "CID did not change"
                        )
                    await asyncio.wait_for(client.ping(), TIMEOUT)
                    for stream, _, _, suffix in requests:
                        client.finish_request(stream, suffix)
                    for stream, _, prefix, suffix in requests:
                        await client.check_response(stream, prefix + suffix, connection)
                assert not nat.errors, nat.errors
                print(
                    f"[PASS] {label}: {rounds} migrations, "
                    f"{STREAMS_PER_ROUND * rounds} complete in-flight requests"
                )
                return connection
            finally:
                client.clear_responses()
    finally:
        nat.close()


async def run(server, rounds, migration_host):
    proc = await asyncio.create_subprocess_exec(server, stdout=asyncio.subprocess.PIPE)
    try:
        line = await asyncio.wait_for(proc.stdout.readline(), TIMEOUT)
        assert line.startswith(b"READY "), line
        port = int(line.split()[1])
        async with asyncio.TaskGroup() as group:
            clients = [
                group.create_task(exercise(port, rounds, label, migration_host))
                for label in ("client-a", "client-b")
            ]
        ids = [task.result() for task in clients]
        assert ids[0] != ids[1], "two clients share connection state"
        assert proc.returncode is None, f"server exited with {proc.returncode}"
        print("[PASS] concurrent clients retain separate connection state")
    finally:
        if proc.returncode is None:
            proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), TIMEOUT)
        except TimeoutError:
            proc.kill()
            await proc.wait()
            raise
    assert proc.returncode == 0, f"server exited with {proc.returncode}"
    print("[PASS] server stopped without a process error")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server")
    parser.add_argument("--rounds", type=int, default=200)
    parser.add_argument(
        "--migration-host",
        default="127.0.0.2",
        help="second configured local IPv4 address",
    )
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error("--rounds must be at least 2")
    if args.migration_host == "127.0.0.1":
        parser.error("--migration-host must differ from 127.0.0.1")
    asyncio.run(
        asyncio.wait_for(run(args.server, args.rounds, args.migration_host), 120)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
