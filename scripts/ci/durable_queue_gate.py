#!/usr/bin/env python3
"""Exercise the real queue test with private, bounded Redis/NATS fixtures."""
import contextlib
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


class GateError(RuntimeError):
    pass


class Port:
    """Reserve an unused loopback port, including during absent-server cases."""
    def __init__(self, number=0):
        self.number = number
        self.sock = None
        self.reserve()

    def reserve(self):
        sock = socket.socket()
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('127.0.0.1', self.number))
        except BaseException:
            sock.close()
            raise
        self.number = sock.getsockname()[1]
        self.sock = sock

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None


def stop(proc):
    """Signal only our live direct child, then reap it with bounded waits."""
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)


def run_case(argv, env, expected, markers, label, timeout=60.0):
    with subprocess.Popen(argv, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True) as proc:
        try:
            output, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise GateError(f'{label}: test timed out') from exc
        finally:
            stop(proc)
    if proc.returncode != expected:
        raise GateError(f'{label}: expected exit {expected}, got {proc.returncode}\n{output}')
    for marker in markers:
        if marker not in output:
            raise GateError(f'{label}: missing diagnostic {marker!r}\n{output}')
    print(f'ok: {label} (exit {proc.returncode})', flush=True)
    print(output, end='', flush=True)


def start_broker(stack, argv, port, log_path, ready_marker, ready_seconds):
    port.close()
    log = stack.enter_context(log_path.open('wb'))
    proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
    stack.callback(stop, proc)
    deadline = time.monotonic() + ready_seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise GateError(f'broker exited {proc.returncode}: {log_path.read_text(errors="replace")}')
        # Require the launched child's own readiness message, not just a port
        # that could have been occupied by another process during startup.
        if ready_marker in log_path.read_text(errors='replace'):
            try:
                with socket.create_connection(('127.0.0.1', port.number), timeout=0.2):
                    return proc
            except OSError:
                pass
        time.sleep(0.05)
    raise GateError(f'broker readiness timeout: {log_path.read_text(errors="replace")}')


def executable(value):
    found = shutil.which(value)
    if not found:
        raise GateError(f'missing executable: {value}')
    return found


def port_number(name):
    value = int(os.environ.get(name, '0'))
    if not 0 <= value <= 65535:
        raise GateError(f'invalid port: {name}')
    return value


def main():
    root = Path(__file__).resolve().parents[2]
    binary = executable(str(root / 'test_durable_queue'))
    redis_bin = executable(os.environ.get('CWIST_DQ_REDIS_BIN', 'redis-server'))
    nats_bin = executable(os.environ.get('CWIST_DQ_NATS_BIN', 'nats-server'))
    ready = float(os.environ.get('CWIST_DQ_READY_SECS', '20'))
    if not 0 < ready <= 120:
        raise GateError('CWIST_DQ_READY_SECS must be in (0, 120]')
    with contextlib.ExitStack() as stack:
        work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='cwist-dq-gate-')))
        ports = []
        for name in ('CWIST_DQ_REDIS_PORT', 'CWIST_DQ_NATS_PORT', 'CWIST_DQ_NOJS_PORT'):
            port = Port(port_number(name))
            stack.callback(port.close)
            ports.append(port)
        redis, nats, plain = ports
        env = dict(os.environ, CWIST_REDIS_HOST='127.0.0.1',
                   CWIST_REDIS_PORT=str(redis.number),
                   CWIST_NATS_URL=f'nats://127.0.0.1:{nats.number}')
        redis_ok = '[durable_queue] Redis backend tests passed.'
        nats_ok = '[durable_queue] NATS backend tests passed.'
        redis_missing = 'REQUIRED: no Redis server'
        nats_missing = 'REQUIRED: no NATS server'

        def case(label, strict, expected, markers, url=None):
            case_env = dict(env)
            case_env.pop('CWIST_TEST_REQUIRE_DURABLE_QUEUE', None)
            if strict is not None:
                case_env['CWIST_TEST_REQUIRE_DURABLE_QUEUE'] = strict
            if url is not None:
                case_env['CWIST_NATS_URL'] = url
            run_case([binary], case_env, expected, markers, label)

        skips = ['skipping redis backend', 'skipping nats backend']
        case('optional, both absent', None, 0, skips)
        case('strict, both absent', '1', 1, [redis_missing, nats_missing])
        r = start_broker(stack, [redis_bin, '--bind', '127.0.0.1', '--port',
                         str(redis.number), '--protected-mode', 'yes', '--save', '',
                         '--appendonly', 'no', '--dir', str(work)], redis,
                         work / 'redis.log', 'Ready to accept connections', ready)
        case('strict, NATS absent', '1', 1, [redis_ok, nats_missing])
        p = start_broker(stack, [nats_bin, '--addr', '127.0.0.1', '-p', str(plain.number)],
                         plain, work / 'plain.log', 'Server is ready', ready)
        case('strict, JetStream disabled', '1', 1,
             [redis_ok, 'REQUIRED: NATS server', 'no JetStream'],
             f'nats://127.0.0.1:{plain.number}')
        stop(p)
        plain.reserve()
        n = start_broker(stack, [nats_bin, '--addr', '127.0.0.1', '-p', str(nats.number),
                         '-js', '--store_dir', str(work / 'jetstream')], nats,
                         work / 'nats.log', 'Server is ready', ready)
        case('strict, both live', '1', 0, [redis_ok, nats_ok, 'backends executed'])
        stop(r)
        redis.reserve()
        case('strict, Redis absent', '1', 1, [redis_missing, nats_ok])
        stop(n)
        nats.reserve()
        case('explicit optional, both absent', '0', 0, skips)
    print('durable-queue gate: all 7 modes passed')


def interrupted(signum, _frame):
    # Let the first signal unwind ExitStack without a second interrupt in cleanup.
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, signal.SIG_IGN)
    raise SystemExit(128 + signum)


if __name__ == '__main__':
    for sig in (signal.SIGINT, signal.SIGTERM):
        if signal.getsignal(sig) != signal.SIG_IGN:
            signal.signal(sig, interrupted)
    try:
        main()
    except (GateError, OSError, ValueError) as error:
        print(f'durable-queue gate: FAIL: {error}', file=sys.stderr)
        sys.exit(1)
