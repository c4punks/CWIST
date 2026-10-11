"""Regression tests for check_api_test_coverage.py on a synthetic repo
layout - no dependency on this repo's actual API baseline or tests/."""
from pathlib import Path
import tempfile
import unittest

import check_api_test_coverage as cov


class CheckApiTestCoverageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tests").mkdir()
        (self.root / "docs/api").mkdir(parents=True)
        (self.root / "scripts/ci").mkdir(parents=True)

    def setup_repo(self, funcs, tests, wired, untested=()):
        (self.root / cov.BASELINE).write_text(
            f"# header\n\n== functions ({len(funcs)}) ==\n" + "\n".join(funcs) +
            "\n\n== types (1) ==\ncwist_app\n")
        for name, body in tests.items():
            (self.root / "tests" / f"{name}.c").write_text(body)
        (self.root / "Makefile").write_text("TEST_TARGETS = " + " ".join(wired) + "\n\ntest:\n")
        (self.root / cov.UNTESTED).write_text("# comment\n" + "\n".join(untested) + "\n")

    def test_all_tested_passes(self):
        self.setup_repo(["cwist_a", "cwist_b"], {"test_x": "cwist_a(); cwist_b();"}, ["test_x"])
        self.assertEqual(cov.check(self.root), ([], [], 2, 0))

    def test_new_untested_function_fails(self):
        self.setup_repo(["cwist_a", "cwist_new"], {"test_x": "cwist_a();"}, ["test_x"])
        self.assertEqual(cov.check(self.root)[0], ["cwist_new"])

    def test_listed_gap_is_allowed(self):
        self.setup_repo(["cwist_a", "cwist_old"], {"test_x": "cwist_a();"}, ["test_x"], ["cwist_old"])
        self.assertEqual(cov.check(self.root)[:2], ([], []))

    def test_listed_but_now_tested_must_be_removed(self):
        self.setup_repo(["cwist_a"], {"test_x": "cwist_a();"}, ["test_x"], ["cwist_a"])
        self.assertEqual(cov.check(self.root)[1], ["cwist_a"])

    def test_unwired_test_does_not_count(self):
        self.setup_repo(["cwist_a"], {"test_x": "cwist_a();"}, [])
        self.assertEqual(cov.check(self.root)[0], ["cwist_a"])

    def test_prefix_name_is_not_a_reference(self):
        self.setup_repo(["cwist_a"], {"test_x": "cwist_a_extra();"}, ["test_x"])
        self.assertEqual(cov.check(self.root)[0], ["cwist_a"])


if __name__ == "__main__":
    unittest.main()
