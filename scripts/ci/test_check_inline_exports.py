"""Regression tests for check_inline_exports.py, on synthetic repo layouts
plus one run against this repository."""
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import check_inline_exports as cie

REPO = Path(__file__).resolve().parents[2]


class CheckInlineExportsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "include" / "cwist" / "vendor").mkdir(parents=True)
        (self.root / "src").mkdir()
        # Synthetic lists instead of the real ones.
        for name, value in (("EXPORTED", {"cw_ok": "cw_ok_extern"}), ("EXCLUDED", {"cw_skip": "x"})):
            patcher = mock.patch.object(cie, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def header(self, rel: str, text: str):
        path = self.root / "include" / "cwist" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def source(self, rel: str, text: str):
        (self.root / "src" / rel).write_text(text)

    def write_good_tree(self):
        self.header("a.h", "static inline bool cw_ok(const int *e) { return !e; }\n"
                           "bool cw_ok_extern(const int *e);\n"
                           "static inline void *cw_skip(void) { return 0; }\n")
        self.source("a.c", "bool cw_ok_extern(const int *e) {\n    return cw_ok(e);\n}\n")

    def test_complete_tree_passes(self):
        self.write_good_tree()
        self.assertEqual(cie.check(self.root), [])

    def test_new_helper_is_reported(self):
        self.write_good_tree()
        self.header("b.h", "static inline int cw_new(int x) { return x; }\n")
        problems = cie.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("cw_new", problems[0])
        self.assertIn("include/cwist/b.h", problems[0])

    def test_vendor_headers_are_ignored(self):
        self.write_good_tree()
        self.header("vendor/v.h", "static inline int vendored(int x) { return x; }\n")
        self.assertEqual(cie.check(self.root), [])

    def test_missing_declaration_is_reported(self):
        self.header("a.h", "static inline bool cw_ok(const int *e) { return !e; }\n"
                           "static inline void *cw_skip(void) { return 0; }\n")
        self.source("a.c", "bool cw_ok_extern(const int *e) {\n    return cw_ok(e);\n}\n")
        self.assertEqual(cie.check(self.root),
                         ["cw_ok_extern() (for cw_ok) is not declared in a public header"])

    def test_missing_definition_is_reported(self):
        self.write_good_tree()
        (self.root / "src" / "a.c").write_text("/* nothing */\n")
        self.assertEqual(cie.check(self.root),
                         ["cw_ok_extern() (for cw_ok) has no non-static definition in src/"])

    def test_static_definition_does_not_count(self):
        # A static wrapper has no external symbol, so it cannot serve a binding.
        self.write_good_tree()
        (self.root / "src" / "a.c").write_text(
            "static bool cw_ok_extern(const int *e) {\n    return cw_ok(e);\n}\n")
        self.assertEqual(cie.check(self.root),
                         ["cw_ok_extern() (for cw_ok) has no non-static definition in src/"])

    def test_stale_entries_are_reported(self):
        self.header("a.h", "int nothing_inline(void);\n")
        self.assertEqual(cie.check(self.root), [
            "EXPORTED entry cw_ok no longer matches a static inline helper",
            "EXCLUDED entry cw_skip no longer matches a static inline helper",
        ])

    def test_repository_passes(self):
        # Uses the real lists against this checkout.
        mock.patch.stopall()
        self.assertEqual(cie.check(REPO), [])


if __name__ == "__main__":
    unittest.main()
