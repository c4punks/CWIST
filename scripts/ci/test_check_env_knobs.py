#!/usr/bin/env python3
"""Unit tests for check_env_knobs.py."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_env_knobs  # noqa: E402

HEADER = "| Variable | Status | Values | Default | Effect |\n|---|---|---|---|---|\n"


def doc(rows: dict[str, str]) -> str:
    return HEADER + "".join(f"| `{n}` | {s} | x | x | x |\n" for n, s in rows.items())


def make_repo(src: str, rows: dict[str, str], ko: dict[str, str] | None = None) -> Path:
    root = Path(tempfile.mkdtemp())
    (root / "src").mkdir()
    (root / "docs/ko").mkdir(parents=True)
    (root / "src/a.c").write_text(src)
    (root / "docs/runtime-config.md").write_text(doc(rows))
    (root / "docs/ko/runtime-config.md").write_text(doc(rows if ko is None else ko))
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    return root


def commit_and_tag(root: Path, tag: str) -> None:
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    subprocess.run(git + ["commit", "-qm", "x"], cwd=root, check=True)
    subprocess.run(["git", "tag", tag], cwd=root, check=True)


SRC = 'x = getenv("CWIST_A"); y = getenv( "CWIST_B" );'


class Coverage(unittest.TestCase):
    def test_matching_sets_pass(self):
        root = make_repo(SRC, {"CWIST_A": "supported", "CWIST_B": "tuning"})
        self.assertEqual(check_env_knobs.check(root, None), [])

    def test_undocumented_knob_fails(self):
        root = make_repo(SRC, {"CWIST_A": "supported"})
        out = check_env_knobs.check(root, None)
        self.assertTrue(any("CWIST_B is read" in f for f in out), out)

    def test_stale_row_fails(self):
        root = make_repo(SRC, {"CWIST_A": "supported", "CWIST_B": "tuning", "CWIST_C": "tuning"})
        out = check_env_knobs.check(root, None)
        self.assertTrue(any("CWIST_C is documented" in f for f in out), out)

    def test_unknown_status_fails(self):
        root = make_repo(SRC, {"CWIST_A": "stable", "CWIST_B": "tuning"})
        out = check_env_knobs.check(root, None)
        self.assertTrue(any("unknown status" in f for f in out), out)


class Mirror(unittest.TestCase):
    def test_missing_and_mismatched_ko_rows_fail(self):
        rows = {"CWIST_A": "supported", "CWIST_B": "tuning"}
        root = make_repo(SRC, rows, ko={"CWIST_A": "tuning"})
        out = check_env_knobs.check(root, None)
        self.assertTrue(any("CWIST_B is missing from docs/ko" in f for f in out), out)
        self.assertTrue(any("CWIST_A: status" in f for f in out), out)


class Guarantee(unittest.TestCase):
    def test_supported_knob_may_not_be_removed_or_demoted(self):
        root = make_repo(SRC, {"CWIST_A": "supported", "CWIST_B": "supported"})
        commit_and_tag(root, "v4")
        (root / "src/a.c").write_text('x = getenv("CWIST_A");')
        rows = {"CWIST_A": "tuning"}
        (root / "docs/runtime-config.md").write_text(doc(rows))
        (root / "docs/ko/runtime-config.md").write_text(doc(rows))
        out = check_env_knobs.check(root, "v4")
        self.assertTrue(any("CWIST_B is supported in v4 but src/ no longer" in f for f in out), out)
        self.assertTrue(any("CWIST_A is supported in v4 but now 'tuning'" in f for f in out), out)

    def test_tuning_knob_may_go_and_missing_ref_is_skipped(self):
        root = make_repo(SRC, {"CWIST_A": "supported", "CWIST_B": "tuning"})
        self.assertEqual(check_env_knobs.check(root, "v4"), [])
        commit_and_tag(root, "v4")
        (root / "src/a.c").write_text('x = getenv("CWIST_A");')
        rows = {"CWIST_A": "supported"}
        (root / "docs/runtime-config.md").write_text(doc(rows))
        (root / "docs/ko/runtime-config.md").write_text(doc(rows))
        self.assertEqual(check_env_knobs.check(root, "v4"), [])


if __name__ == "__main__":
    unittest.main()
