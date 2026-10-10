"""Regression tests for the `cwist` CLI's ownership-hazard warnings
(HAZARDS / scan_hazards / report_hazards), on synthetic source trees.

The warning exists because cwist_gc_scope_flush() from a request middleware
was silent in production (c4punks/CWIST#347): these tests pin that such a
call is reported, that an explicit `cwist-ack:` marker is the only way to
silence it, and that CWIST's own sources are not flagged."""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import io
import tempfile
import unittest

_CLI = Path(__file__).resolve().parents[2] / "tools" / "cli" / "cwist"
_spec = spec_from_loader("cwist_cli", SourceFileLoader("cwist_cli", str(_CLI)))
cwist = module_from_spec(_spec)
_spec.loader.exec_module(cwist)


class HazardScanTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "src").mkdir()

    def write_src(self, name: str, body: str) -> Path:
        path = self.root / "src" / name
        path.write_text(body, encoding="utf-8")
        return path

    def apis(self) -> list[tuple[int, str]]:
        return [(ln, api) for _, ln, api in cwist.scan_hazards(self.root / "src")]

    def test_middleware_flush_is_reported(self):
        self.write_src("main.c",
                       "static void mw(void *req, void *res, void (*next)(void *, void *)) {\n"
                       "    next(req, res);\n"
                       "    cwist_gc_scope_flush();\n"
                       "}\n")
        self.assertEqual(self.apis(), [(3, "cwist_gc_scope_flush")])

    def test_every_hazard_is_detected_in_c_and_h(self):
        self.write_src("a.c", "".join(f"void f{i}(void) {{ {api}(p); }}\n"
                                      for i, api in enumerate(cwist.HAZARDS)))
        self.write_src("b.h", "static inline void g(void) { cwist_ebr_free(p); }\n")
        found = {api for _, api in self.apis()}
        self.assertEqual(found, set(cwist.HAZARDS))
        self.assertEqual(sum(1 for _, api in self.apis() if api == "cwist_ebr_free"), 2)

    def test_ack_on_same_line_or_line_above_silences(self):
        self.write_src("job.c",
                       "void job_done(void) {\n"
                       "    cwist_gc_scope_flush(); /* cwist-ack: cwist_gc_scope_flush */\n"
                       "    /* cwist-ack: cwist_conn_registry_flush */\n"
                       "    cwist_conn_registry_flush();\n"
                       "}\n")
        self.assertEqual(self.apis(), [])

    def test_ack_for_a_different_api_does_not_silence(self):
        self.write_src("job.c",
                       "void f(void) {\n"
                       "    cwist_gc_scope_flush(); /* cwist-ack: cwist_gc_scope_disown */\n"
                       "}\n")
        self.assertEqual(self.apis(), [(2, "cwist_gc_scope_flush")])

    def test_one_marker_can_ack_several_apis(self):
        self.write_src("job.c",
                       "/* cwist-ack: cwist_gc_scope_disown, cwist_gc_scope_flush */\n"
                       "cwist_gc_scope_disown(p); cwist_gc_scope_flush();\n")
        self.assertEqual(self.apis(), [])

    def test_comments_and_prefixed_names_are_ignored(self):
        self.write_src("doc.c",
                       "// cwist_gc_scope_flush() must not run here\n"
                       " * cwist_gc_scope_flush() in a block comment\n"
                       "void my_cwist_gc_scope_flush_wrapper(void);\n"
                       "size_t n = cwist_gc_scope_pending_count();\n")
        self.assertEqual(self.apis(), [])

    def test_cwist_own_tree_is_skipped(self):
        (self.root / "include" / "cwist" / "core" / "mem").mkdir(parents=True)
        (self.root / "include" / "cwist" / "core" / "mem" / "gc.h").write_text("", encoding="utf-8")
        (self.root / "src" / "core" / "mem").mkdir(parents=True)
        (self.root / "src" / "core" / "mem" / "gc.c").write_text("", encoding="utf-8")
        self.write_src("internal.c", "void f(void) { cwist_gc_scope_flush(); }\n")
        self.assertEqual(self.apis(), [])

    def test_report_names_the_contract_and_the_ack_marker(self):
        path = self.write_src("main.c", "void f(void) { cwist_gc_scope_flush(); }\n")
        out = io.StringIO()
        cwist.report_hazards(cwist.scan_hazards(path), out)
        text = out.getvalue()
        self.assertIn("WARNING: cwist_gc_scope_flush()", text)
        self.assertIn("never from a handler, middleware", text)
        self.assertIn("/* cwist-ack: cwist_gc_scope_flush */", text)
        self.assertIn("docs/GC.md", text)
        self.assertIn("main.c:1", text)

    def test_no_hits_prints_nothing(self):
        path = self.write_src("main.c", "int main(void) { return 0; }\n")
        out = io.StringIO()
        cwist.report_hazards(cwist.scan_hazards(path), out)
        self.assertEqual(out.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
