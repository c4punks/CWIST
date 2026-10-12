#!/usr/bin/env python3
"""Unit tests for api_baseline.py and check_api_baseline.py."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import api_baseline  # noqa: E402
import check_api_baseline  # noqa: E402


def make_repo(header: str) -> Path:
    root = Path(tempfile.mkdtemp())
    (root / "include/cwist").mkdir(parents=True)
    (root / "include/cwist/x.h").write_text(textwrap.dedent(header))
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    return root


class ParamNames(unittest.TestCase):
    def test_names_dropped_types_kept(self):
        f = api_baseline.strip_param_name
        self.assertEqual(f("const char *path"), "const char *")
        self.assertEqual(f("unsigned int"), "unsigned int")
        self.assertEqual(f("size_t len"), "size_t")
        self.assertEqual(f("char **out"), "char **")
        self.assertEqual(f("uint8_t out[32]"), "uint8_t[]")
        self.assertEqual(f("void (*cb)(void *ctx)"), "void (*)(void *ctx)")
        self.assertEqual(f("struct cwist_app"), "struct cwist_app")
        self.assertEqual(f("void"), "void")


class Generator(unittest.TestCase):
    def test_declaration_after_directive_is_found(self):
        root = make_repo("""
            #include <stddef.h>
            void cwist_after_include(int a);
            #define CWIST_X 1
            int cwist_after_define(const char *s);
        """)
        c = api_baseline.collect(root)
        self.assertEqual(c["functions"], {"cwist_after_include", "cwist_after_define"})
        self.assertIn("cwist_after_define: int (const char *)", c["signatures"])

    def test_calls_in_inline_bodies_are_not_declarations(self):
        root = make_repo("""
            void cwist_real(int a);
            static inline void cwist_helper(int a) { cwist_real(a); cwist_other(a); }
        """)
        c = api_baseline.collect(root)
        self.assertEqual(c["functions"], {"cwist_real"})
        self.assertEqual(c["inline_signatures"], {"cwist_helper: void (int)"})

    def test_layout_keeps_member_order_and_conditionals(self):
        root = make_repo("""
            typedef struct cwist_s {
                int a;   /* comment */
            #ifdef CWIST_FEATURE
                char *b;
            #endif
                bool c;
            } cwist_s;
            typedef enum { CWIST_A = 1, CWIST_B } cwist_e;
        """)
        layouts = api_baseline.collect(root)["layouts"]
        self.assertEqual(layouts["struct cwist_s"],
                         ["int a", "#ifdef CWIST_FEATURE", "char *b", "#endif", "bool c"])
        self.assertEqual(layouts["enum cwist_e"], ["CWIST_A = 1", "CWIST_B"])


class Gate(unittest.TestCase):
    OLD = textwrap.dedent("""\
        # header
        == functions (2) ==
        cwist_a
        cwist_b
        == layouts (1) ==
        struct cwist_s [0] int a

        # Experimental
        == experimental functions (1) ==
        cwist_webtransport_x
        """)

    def test_additions_pass_removals_fail(self):
        added = self.OLD.replace("cwist_b\n", "cwist_b\ncwist_c\n")
        self.assertEqual(check_api_baseline.removed_guaranteed(self.OLD, added), [])
        removed = self.OLD.replace("cwist_b\n", "")
        self.assertEqual(check_api_baseline.removed_guaranteed(self.OLD, removed),
                         ["functions: cwist_b"])

    def test_changed_member_fails_experimental_ignored(self):
        new = self.OLD.replace("int a", "long a").replace("cwist_webtransport_x\n", "")
        self.assertEqual(check_api_baseline.removed_guaranteed(self.OLD, new),
                         ["layouts: struct cwist_s [0] int a"])


if __name__ == "__main__":
    unittest.main()
