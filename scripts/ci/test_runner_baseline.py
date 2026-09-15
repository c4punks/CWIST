import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runner_baseline import runner_key, same_runner_values, evaluate, summarize


def row(hw, lat, rps=None):
    r = {'runner_hw': hw, 'cwist_c1m_lat_ms': lat}
    if rps is not None:
        r['cwist_c1m_rps'] = rps
    return r


FAST = '4 vCPU | Intel(R) Xeon(R) 6973P-C'
SLOW = '4 vCPU | AMD EPYC 7763 64-Core Processor'


class RunnerKeyTests(unittest.TestCase):
    def test_model_after_the_bar_is_the_key(self):
        self.assertEqual(runner_key({'runner_hw': SLOW}), 'AMD EPYC 7763 64-Core Processor')

    def test_missing_or_blank_is_none(self):
        for hw in (None, '', '   ', 5):
            self.assertIsNone(runner_key({'runner_hw': hw}))
        self.assertIsNone(runner_key({}))

    def test_values_are_filtered_by_runner_and_validity(self):
        history = [row(SLOW, 3.0), row(FAST, 1.9), row(SLOW, 3.1),
                   row(SLOW, 0), row(SLOW, None), {'runner_hw': SLOW}]
        self.assertEqual(same_runner_values(history, 'AMD EPYC 7763 64-Core Processor',
                                            'cwist_c1m_lat_ms'), [3.0, 3.1])


class EvaluateTests(unittest.TestCase):
    def slow_history(self, n=6, value=3.0):
        return [row(SLOW, value) for _ in range(n)]

    def test_typical_run_on_a_known_cpu_passes(self):
        ok, detail = evaluate(self.slow_history(), row(SLOW, 3.1), 'cwist_c1m_lat_ms', 3.5)
        self.assertTrue(ok)
        self.assertIn('within', detail)

    def test_regression_against_same_cpu_fails_even_under_the_ceiling(self):
        # 3.4ms passes the 3.5ms absolute gate but is 13% over this CPU's
        # own 3.0ms median times the allowance, which is the case the
        # absolute-only gate used to wave through.
        ok, detail = evaluate(self.slow_history(), row(SLOW, 4.2), 'cwist_c1m_lat_ms', 5.0)
        self.assertFalse(ok)
        self.assertIn('median', detail)

    def test_ceiling_still_fails_regardless_of_history(self):
        ok, detail = evaluate(self.slow_history(), row(SLOW, 9.0), 'cwist_c1m_lat_ms', 3.5)
        self.assertFalse(ok)
        self.assertIn('ceiling', detail)

    def test_fast_cpu_is_not_judged_against_slow_cpu_history(self):
        # 2.4ms on the fast CPU is a real regression from its own ~1.9ms,
        # but would look healthy next to the slow CPU's 3.0ms.
        history = self.slow_history() + [row(FAST, 1.9) for _ in range(6)]
        ok, _ = evaluate(history, row(FAST, 2.8), 'cwist_c1m_lat_ms', 3.5)
        self.assertFalse(ok)

    def test_unknown_cpu_falls_back_to_the_ceiling(self):
        ok, detail = evaluate(self.slow_history(), row('4 vCPU | Brand New Chip', 3.4),
                              'cwist_c1m_lat_ms', 3.5)
        self.assertTrue(ok)
        self.assertIn('ceiling only', detail)

    def test_too_few_samples_falls_back_to_the_ceiling(self):
        ok, detail = evaluate(self.slow_history(n=2), row(SLOW, 3.4), 'cwist_c1m_lat_ms', 3.5)
        self.assertTrue(ok)
        self.assertIn('earlier run', detail)

    def test_missing_metric_is_skipped_not_failed(self):
        ok, detail = evaluate(self.slow_history(), {'runner_hw': SLOW}, 'cwist_c1m_lat_ms', 3.5)
        self.assertTrue(ok)
        self.assertIn('not recorded', detail)

    def test_row_without_runner_uses_the_ceiling(self):
        ok, _ = evaluate(self.slow_history(), {'cwist_c1m_lat_ms': 3.4}, 'cwist_c1m_lat_ms', 3.5)
        self.assertTrue(ok)
        ok, _ = evaluate(self.slow_history(), {'cwist_c1m_lat_ms': 9.0}, 'cwist_c1m_lat_ms', 3.5)
        self.assertFalse(ok)


class SummarizeTests(unittest.TestCase):
    def test_groups_by_cpu_and_orders_by_sample_count(self):
        history = [row(SLOW, 3.0, 140000), row(SLOW, 3.2, 142000),
                   row(SLOW, 3.1, 141000), row(FAST, 1.9, 290000)]
        out = summarize(history, ['cwist_c1m_lat_ms', 'cwist_c1m_rps'])
        self.assertEqual([o[0] for o in out],
                         ['AMD EPYC 7763 64-Core Processor', 'Intel(R) Xeon(R) 6973P-C'])
        self.assertEqual(out[0][1], 3)
        self.assertAlmostEqual(out[0][2]['cwist_c1m_lat_ms'], 3.1)
        self.assertAlmostEqual(out[1][2]['cwist_c1m_rps'], 290000)

    def test_rows_without_a_runner_are_left_out(self):
        self.assertEqual(summarize([{'cwist_c1m_lat_ms': 3.0}], ['cwist_c1m_lat_ms']), [])


if __name__ == '__main__':
    unittest.main()
