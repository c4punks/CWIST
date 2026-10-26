#!/usr/bin/env python3
"""Run the distributed limiter checks against a fresh loopback Redis.

No connection is made to an existing Redis. A missing redis-server or a
failed startup is an error, never a skipped integration check.
"""

import argparse
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def ready(port, process):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("owned Redis exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2) as sock:
                sock.sendall(b"*1\r\n$4\r\nPING\r\n")
                reply = b""
                while not reply.endswith(b"\r\n") and len(reply) < 64:
                    chunk = sock.recv(64)
                    if not chunk:
                        break
                    reply += chunk
                if reply == b"+PONG\r\n":
                    return
        except OSError:
            pass
        time.sleep(0.02)
    raise TimeoutError("owned Redis did not become ready in 10 seconds")


def run(binary, redis_server):
    executable = shutil.which(redis_server)
    if not executable:
        raise RuntimeError("redis-server is required for this integration test")
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    parent = os.environ.get("RUNNER_TEMP") or os.environ.get("TMPDIR") or "."
    with tempfile.TemporaryDirectory(
        prefix="cwist-redis-rate-", dir=parent
    ) as directory:
        log = Path(directory) / "redis.log"
        redis = subprocess.Popen(
            [
                executable,
                "--bind",
                "127.0.0.1",
                "--port",
                str(port),
                "--protected-mode",
                "yes",
                "--save",
                "",
                "--appendonly",
                "no",
                "--daemonize",
                "no",
                "--dir",
                directory,
                "--logfile",
                str(log),
            ],
            stdout=subprocess.DEVNULL,
        )
        test = None
        try:
            ready(port, redis)
            env = dict(os.environ)
            env["CWIST_REDIS_RATE_LIMIT_TEST_PORT"] = str(port)
            env["CWIST_REDIS_RATE_LIMIT_REQUIRE"] = "1"
            test = subprocess.Popen(
                [str(Path(binary).resolve())], env=env, start_new_session=True
            )
            try:
                code = test.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(test.pid, signal.SIGKILL)
                test.wait()
                raise TimeoutError("limiter test exceeded 30 seconds") from None
            if code:
                raise RuntimeError(f"limiter test failed with exit {code}")
            print("[PASS] strict owned-Redis two-process integration completed")
        except BaseException:
            if test is not None:
                # The test may have forked workers before its parent failed.
                try:
                    os.killpg(test.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                test.wait()
            if log.exists():
                print(log.read_text()[-2000:])
            raise
        finally:
            redis.terminate()
            try:
                redis.wait(timeout=5)
            except subprocess.TimeoutExpired:
                redis.kill()
                redis.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--redis-server", default="redis-server")
    args = parser.parse_args()
    run(args.binary, args.redis_server)


if __name__ == "__main__":
    main()
