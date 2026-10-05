import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from https_gates_eval import HISTORY, evaluate, with_ratios

RYZEN = '12 vCPU | AMD Ryzen 5 5600X 6-Core Processor'
EPYC = '4 vCPU | AMD EPYC 7763 64-Core Processor'

# Measured on the same Ryzen 5600X, same tree, minutes apart: the second row
# with #307's TCP_QUICKACK re-arm disabled (the regression the gates exist
# to catch).
HEALTHY = {
    'timestamp': '2026-10-05T20:05:42+0900', 'runner_hw': RYZEN,
    'tls_rtt_p50_ms': 0.083, 'https_churn_rps': 1664.86, 'http_churn_rps': 14080.51,
    'https_keepalive_rps': 182125.92, 'http_keepalive_rps': 278394.62,
    'https_big_gbps': 4.2, 'http_big_gbps': 12.97,
}
PRE_307 = {
    'timestamp': '2026-10-05T20:07:12+0900', 'runner_hw': RYZEN,
    'tls_rtt_p50_ms': 43.04, 'https_churn_rps': 677.46, 'http_churn_rps': 13051.42,
    'https_keepalive_rps': 164250.42, 'http_keepalive_rps': 233103.08,
    'https_big_gbps': 4.03, 'http_big_gbps': 13.43,
}


def failed_metrics(failures):
    return {line.split(':', 1)[0] for line in failures}


def epyc_row(i, churn=1180.0):
    return {
        'timestamp': f'2026-10-0{i}T00:00:00+0000', 'runner_hw': EPYC,
        'tls_rtt_p50_ms': 0.12, 'https_churn_rps': churn, 'http_churn_rps': 9600.0,
        'https_keepalive_rps': 76000.0, 'http_keepalive_rps': 129000.0,
        'https_big_gbps': 1.95, 'http_big_gbps': 5.6,
    }


class GateTests(unittest.TestCase):
    def test_healthy_tree_passes(self):
        failures, _ = evaluate([], HEALTHY)
        self.assertEqual(failures, [])

    def test_pre_307_regression_fails_on_rtt(self):
        failures, _ = evaluate([], PRE_307)
        self.assertIn('tls_rtt_p50_ms', failed_metrics(failures))

    def test_pre_307_regression_fails_same_cpu_churn_ratio(self):
        # With Ryzen history, the churn-ratio drop (0.118 -> 0.052) fails the
        # same-CPU check even where the absolute backstop is too loose to.
        history = [dict(HEALTHY, timestamp=f'2026-10-0{i}') for i in range(1, 6)]
        failures, _ = evaluate(history, dict(PRE_307, tls_rtt_p50_ms=0.1))
        self.assertIn('https_churn_ratio', failed_metrics(failures))

    def test_healthy_intel_runner_passes_without_history(self):
        # Run 37301931499: lower churn ratio than any AMD runner, healthy code.
        intel = {
            'runner_hw': '4 vCPU | INTEL(R) XEON(R) PLATINUM 8573C', 'tls_rtt_p50_ms': 0.08,
            'https_churn_rps': 1440.53, 'http_churn_rps': 20430.4,
            'https_keepalive_rps': 143567.15, 'http_keepalive_rps': 213915.29,
            'https_big_gbps': 3.37, 'http_big_gbps': 7.96,
        }
        failures, _ = evaluate([], intel)
        self.assertEqual(failures, [])

    def test_every_recorded_run_passes_against_the_rest(self):
        history = json.loads(HISTORY.read_text())
        self.assertGreaterEqual(len(history), 5)
        for i, row in enumerate(history):
            failures, _ = evaluate(history[:i] + history[i + 1:], row)
            self.assertEqual(failures, [], f"{row['timestamp']} {row['runner_hw']}")

    def test_same_cpu_check_engages_after_five_runs(self):
        history = [epyc_row(i) for i in range(1, 6)]
        # 25% lower churn: above every absolute backstop and the ratio
        # backstop, but under the same-CPU median / 1.25.
        failures, details = evaluate(history, epyc_row(7, churn=880.0))
        self.assertIn('https_churn_rps', failed_metrics(failures))
        self.assertTrue(any('within same-CPU gate' in d for d in details))

    def test_ratios_are_derived_for_old_rows(self):
        row = with_ratios({'https_churn_rps': 100.0, 'http_churn_rps': 1000.0})
        self.assertEqual(row['https_churn_ratio'], 0.1)
        self.assertNotIn('https_big_ratio', row)


if __name__ == '__main__':
    unittest.main()
