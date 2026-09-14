"""Producer/consumer regression using actual emitted telemetry, not success flags."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch
import benchmark_workload
from webserver_result import parse_wrk_text, validate_case
from test_webserver_result import sample, output


class HandoffTests(unittest.TestCase):
    def run_case(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            connection = Mock()
            connection.getresponse.return_value.status = 200
            connection.getresponse.return_value.read.return_value = b'ok'
            if mode == 'read-error':
                connection.getresponse.return_value.read.side_effect = OSError('synthetic read failure')
            rows = [{'pid': 500, 'start': '42', 'tasks': [], 'rss_kib': 10}]

            def execute(command, stdout, **kwargs):
                measured = '--latency' in command
                if measured:
                    stdout.write(output(sample()))
                else:
                    stdout.write('200000 requests in 10.0s, 1MB read\n')
                    if mode == 'warm-error':
                        stdout.write('Socket errors: connect 1, read 0, write 0, timeout 0\n')
                return subprocess.CompletedProcess(command, 7 if measured and mode == 'load-error' else 0)

            args = ['9091', str(root/'load.txt'), str(root/'stat.json'), '1', '12', '400']
            with patch.dict(os.environ, {'BENCHMARK_SERVER_PID': '500'}), \
                 patch('benchmark_workload.snapshot_group', return_value=rows), \
                 patch('http.client.HTTPConnection', return_value=connection), \
                 patch('subprocess.run', side_effect=execute), \
                 patch('time.monotonic', side_effect=[0, 2, 12]):
                if mode == 'ok':
                    self.assertEqual(benchmark_workload.main(args), 0)
                else:
                    with self.assertRaises((ValueError, subprocess.CalledProcessError, RuntimeError)):
                        benchmark_workload.main(args)
            telemetry = json.loads((root/'stat.json').read_text())
            metrics = parse_wrk_text((root/'load.txt').read_text() if mode == 'ok' else output(sample()))
            receipt = {'complete': True, 'cleanup_ok': True, 'survivors': []}
            if mode == 'ok':
                validate_case(metrics, receipt, telemetry)
                self.assertTrue(telemetry['complete'])
            else:
                self.assertFalse(telemetry['complete'])
                with self.assertRaises(ValueError):
                    validate_case(metrics, receipt, telemetry)
            return telemetry

    def test_actual_successful_handoff(self):
        data = self.run_case('ok')
        self.assertEqual(data['measurement_exit'], 0)

    def test_read_failure_is_not_ready(self):
        data = self.run_case('read-error')
        self.assertFalse(data['ready'])
        self.assertFalse(data['warmup_ok'])
        self.assertIsNone(data['measurement_exit'])

    def test_warm_failure_has_no_measurement(self):
        data = self.run_case('warm-error')
        self.assertTrue(data['ready'])
        self.assertFalse(data['warmup_ok'])
        self.assertIsNone(data['measurement_exit'])

    def test_actual_nonzero_exit_preserved(self):
        data = self.run_case('load-error')
        self.assertTrue(data['ready'])
        self.assertTrue(data['warmup_ok'])
        self.assertEqual(data['measurement_exit'], 7)


if __name__ == '__main__':
    unittest.main()
