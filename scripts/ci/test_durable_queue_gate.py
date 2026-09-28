"""Behavioral tests of the real gate helpers; no Redis/NATS required."""
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

import durable_queue_gate as gate


class GateTests(unittest.TestCase):
    def invoke(self, code, expected=1, markers=('REQUIRED',), timeout=2.0):
        gate.run_case([sys.executable, '-c', code], os.environ.copy(),
                      expected, markers, 'fixture', timeout=timeout)

    def test_exact_exit_and_diagnostic(self):
        self.invoke("print('REQUIRED'); raise SystemExit(1)")
        for code in (0, 2, 139):
            with self.subTest(code=code), self.assertRaisesRegex(gate.GateError, 'expected exit 1'):
                self.invoke(f"print('REQUIRED'); raise SystemExit({code})")
        with self.assertRaisesRegex(gate.GateError, 'missing diagnostic'):
            self.invoke('raise SystemExit(1)')

    def test_signal_death_is_not_expected_failure(self):
        with self.assertRaisesRegex(gate.GateError, 'expected exit 1'):
            self.invoke("import os,signal; print('REQUIRED',flush=True); os.kill(os.getpid(),signal.SIGTERM)")

    def test_timeout_terminates_and_reaps_test(self):
        with tempfile.TemporaryDirectory() as td:
            pidfile = Path(td) / 'pid'
            code = f"import os,time,pathlib; pathlib.Path({str(pidfile)!r}).write_text(str(os.getpid())); time.sleep(60)"
            with self.assertRaisesRegex(gate.GateError, 'timed out'):
                self.invoke(code, timeout=0.5)
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pidfile.read_text()), 0)

    def test_occupied_port_refused_without_connecting(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            with self.assertRaises(OSError):
                gate.Port(listener.getsockname()[1])
            listener.settimeout(0.05)
            with self.assertRaises(socket.timeout):
                listener.accept()

    def test_failed_broker_readiness_cleans_child(self):
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / 'broker.log'
            with self.assertRaisesRegex(gate.GateError, 'readiness timeout'):
                with contextlib.ExitStack() as stack:
                    port = gate.Port()
                    stack.callback(port.close)
                    gate.start_broker(stack, [sys.executable, '-u', '-c',
                        "import os,time; print(os.getpid()); time.sleep(60)"],
                        port, log, 'never ready', 0.3)
            with self.assertRaises(ProcessLookupError):
                os.kill(int(log.read_text().strip()), 0)


if __name__ == '__main__':
    unittest.main()
