"""Regression tests for `cwist audit --gate`'s raw-allocator baseline, on a
synthetic src/ tree - no dependency on this repo's actual sources.

The gate keys call sites on file + code rather than file + line number.
These tests pin both halves of that trade: line drift must NOT look like a
new allocation, and dropping the line number must NOT cost the ability to
spot one more copy of a line that already appears in the file."""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import contextlib
import io
import tempfile
import unittest

_CLI = Path(__file__).resolve().parents[2] / "tools" / "cli" / "cwist"
_spec = spec_from_loader("cwist_cli", SourceFileLoader("cwist_cli", str(_CLI)))
cwist = module_from_spec(_spec)
_spec.loader.exec_module(cwist)


class MallocGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "src").mkdir()
        self.baseline = self.root / "baseline.txt"

    def write_src(self, name: str, body: str):
        (self.root / "src" / name).write_text(body, encoding="utf-8")

    def pin(self):
        """Record the tree's current call sites as the accepted baseline."""
        with contextlib.redirect_stdout(io.StringIO()):
            cwist.audit_update_baseline(self.root, self.baseline)

    def gate(self) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = cwist.audit_gate(self.root, self.baseline)
        return rc, out.getvalue()

    # --- the regression this format change exists for --------------------

    def test_line_drift_is_not_a_new_allocation(self):
        self.write_src("a.c", "void f(void) {\n    free(p);\n}\n")
        self.pin()
        # An unrelated edit above the call site, exactly like the three
        # security-header lines that shifted http.c's strdup 1585 -> 1588.
        self.write_src("a.c", "void added(void) {\n    return;\n}\n\n"
                              "void f(void) {\n    free(p);\n}\n")
        self.assertEqual(self.gate(), (0, ""))

    def test_reindentation_is_not_a_new_allocation(self):
        self.write_src("a.c", "    free(p);\n")
        self.pin()
        self.write_src("a.c", "            free(p);\n")
        self.assertEqual(self.gate(), (0, ""))

    # --- what the gate must still catch ----------------------------------

    def test_new_call_site_fails(self):
        self.write_src("a.c", "    free(p);\n")
        self.pin()
        self.write_src("a.c", "    free(p);\n    char *q = strdup(s);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 1)
        self.assertIn("a.c:char *q = strdup(s);", out)

    def test_extra_copy_of_a_pinned_line_fails(self):
        """Dropping the line number must not collapse duplicates: orm.c
        frees `qtable` in seven error paths, and an eighth is still new."""
        self.write_src("a.c", "    free(q);\n    free(q);\n")
        self.pin()
        self.write_src("a.c", "    free(q);\n    free(q);\n    free(q);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 1)
        self.assertIn("[2 -> 3 in this file]", out)

    def test_same_text_in_another_file_fails(self):
        self.write_src("a.c", "    free(q);\n")
        self.pin()
        self.write_src("b.c", "    free(q);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 1)
        self.assertIn("b.c:free(q);", out)

    # --- shrinkage is a note, not a failure ------------------------------

    def test_removed_call_site_is_a_tighten_note(self):
        self.write_src("a.c", "    free(p);\n    char *q = strdup(s);\n")
        self.pin()
        self.write_src("a.c", "    free(p);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 0)
        self.assertIn("no longer present", out)
        self.assertIn("a.c:char *q = strdup(s);", out)

    def test_one_of_several_copies_removed_is_a_tighten_note(self):
        self.write_src("a.c", "    free(q);\n    free(q);\n")
        self.pin()
        self.write_src("a.c", "    free(q);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 0)
        self.assertIn("[2 -> 1 in this file]", out)

    # --- scan rules carried over from the old gate -----------------------

    def test_cwist_wrapped_and_commented_calls_are_ignored(self):
        self.write_src("a.c", "    cwist_free(p);\n"
                              "    char *s = cwist_strdup(t);\n"
                              "    // free(p);\n"
                              "    /* malloc(1) */\n"
                              "     * free(p);\n")
        self.pin()
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), "")
        self.assertEqual(self.gate(), (0, ""))

    def test_substring_names_are_not_matched(self):
        self.write_src("a.c", "    my_free(p);\n    xfree(p);\n")
        self.pin()
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), "")

    # --- format handling --------------------------------------------------

    def test_retired_line_pinned_baseline_is_rejected(self):
        self.write_src("a.c", "    free(p);\n")
        self.baseline.write_text("src/a.c:2:    free(p);\n", encoding="utf-8")
        rc, out = self.gate()
        self.assertEqual(rc, 1)
        self.assertIn("--update-baseline", out)

    def test_missing_baseline_reports_every_site_as_new(self):
        self.write_src("a.c", "    free(p);\n")
        rc, out = self.gate()
        self.assertEqual(rc, 1)
        self.assertIn("a.c:free(p);", out)

    def test_baseline_roundtrips(self):
        self.write_src("a.c", "    free(q);\n    free(q);\n    char *s = strdup(t);\n")
        self.pin()
        self.assertEqual(self.gate(), (0, ""))
        counts, malformed = cwist.parse_baseline(self.baseline)
        self.assertEqual(malformed, [])
        self.assertEqual(counts["src/a.c:free(q);"], 2)
        self.assertEqual(sum(counts.values()), 3)


if __name__ == "__main__":
    unittest.main()
