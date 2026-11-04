import re
import subprocess
import tempfile
import unittest
from pathlib import Path


class SeqAuthJavaScriptTests(unittest.TestCase):
    def test_em_js_entropy_fallback_parses(self):
        source = Path(__file__).parents[2].joinpath("src/core/seq/seq_auth.c").read_text()
        match = re.search(r"EM_JS\(int, seq_auth_random_js,.*?\{(.*?)// clang-format on", source, re.S)
        self.assertIsNotNone(match)
        with tempfile.NamedTemporaryFile("w", suffix=".js") as script:
            script.write(match.group(1))
            script.flush()
            result = subprocess.run(["node", "--check", script.name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
