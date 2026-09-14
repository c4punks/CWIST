"""Keep the real scheduler/queue GC regression in release test gates."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class JobHandoffWiringTests(unittest.TestCase):
    def test_default_test_suite_includes_handoff_regression(self):
        text = (ROOT / 'Makefile').read_text()
        targets = text.split('TEST_TARGETS = ', 1)[1].split('\n\n', 1)[0]
        self.assertIn('test_gc_job_handoff', targets.split())
        match = re.search(r'^test_gc_job_handoff:([^\n]*)\n((?:\t[^\n]*\n)+)',
                          text, re.M)
        if match is None:
            self.fail('missing executable handoff test target')
        prerequisites, recipe = match.groups()
        self.assertIn('$(LIB_NAME)', prerequisites)
        self.assertIn('tests/test_gc_job_handoff.c', prerequisites)
        self.assertIn('-DNDEBUG', recipe)
        self.assertGreaterEqual(recipe.count('\t./$@\n'), 2)

    def test_macos_focused_suite_runs_handoff_regression(self):
        text = (ROOT / '.github/workflows/bsd-kqueue-benchmarks.yml').read_text()
        job = text.split('  bsd-kqueue:\n', 1)[1].split('\n  benchmark:', 1)[0]
        commands = '\n'.join(line.strip() for line in job.splitlines()
                             if line.strip().startswith('make '))
        self.assertRegex(commands, r'\btest_gc_job_handoff\b')


if __name__ == '__main__':
    unittest.main()
