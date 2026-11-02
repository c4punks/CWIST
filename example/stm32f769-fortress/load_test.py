#!/usr/bin/env python3
import argparse
import socket
import threading
import time


def exchange(host, port, path, pace, index, results):
    start = time.monotonic() + pace * index
    delay = start - time.monotonic()
    if delay > 0:
        time.sleep(delay)
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=10)
        sock.settimeout(120)
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Connection: keep-alive\r\n\r\n"
        ).encode("ascii")
        sock.sendall(request)
        data = bytearray()
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed before response headers")
            data.extend(chunk)
        head, body = bytes(data).split(b"\r\n\r\n", 1)
        body = bytearray(body)
        status = int(head.split(b" ", 2)[1])
        length = None
        for line in head.split(b"\r\n")[1:]:
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1].strip())
        if status != 200 or length is None:
            raise ConnectionError(f"unexpected response: {head!r}")
        while len(body) < length:
            chunk = sock.recv(min(65536, length - len(body)))
            if not chunk:
                raise ConnectionError(f"truncated body: {len(body)}/{length}")
            body.extend(chunk)
        results[index] = (True, length, sock)
    except Exception as exc:
        if sock is not None:
            sock.close()
        results[index] = (False, str(exc), None)


def main():
    parser = argparse.ArgumentParser(description="Hold concurrent CWIST connections and verify every response")
    parser.add_argument("host", help="board IPv4 address")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("-n", "--connections", type=int, default=288)
    parser.add_argument("--path", default="/api/stats", choices=("/api/stats", "/stream"))
    parser.add_argument("--pace", type=float, default=0.05, help="seconds between connection starts")
    parser.add_argument("--hold", type=float, default=2, help="seconds to keep successful sockets open")
    args = parser.parse_args()
    if args.connections < 1 or args.connections > 1024:
        parser.error("connections must be between 1 and 1024")

    results = [None] * args.connections
    threads = []
    started = time.monotonic()
    for i in range(args.connections):
        thread = threading.Thread(target=exchange, args=(args.host, args.port, args.path, args.pace, i, results))
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    elapsed = time.monotonic() - started
    successful = [result for result in results if result and result[0]]
    failed = [(i, result[1]) for i, result in enumerate(results) if result and not result[0]]
    print(f"connections={args.connections} succeeded={len(successful)} failed={len(failed)} elapsed={elapsed:.2f}s")
    if successful:
        sizes = [result[1] for result in successful]
        print(f"response_bytes min={min(sizes)} max={max(sizes)} total={sum(sizes)}")
        print(f"holding {len(successful)} sockets for {args.hold:.1f}s")
        time.sleep(args.hold)
    for result in successful:
        result[2].close()
    for index, error in failed[:20]:
        print(f"failure connection={index + 1}: {error}")
    if len(failed) > 20:
        print(f"and {len(failed) - 20} more failures")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
