"""Release artifact and native cache test wiring contracts (stdlib only)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReleaseWorkflowTests(unittest.TestCase):
    def test_uploaded_source_archive_matches_packaged_version(self):
        text = (ROOT / '.github/workflows/source-distribution.yml').read_text()
        match = re.search(r'^  RELEASE_VERSION: (\S+)$', text, re.M)
        if match is None:
            self.fail('release version is missing')
        version = match.group(1)
        upload = text.split('uses: actions/upload-artifact@', 1)[1]
        resolved = upload.replace('${{ env.RELEASE_VERSION }}', version)
        paths = re.findall(r'^\s+(dist/\S+)\s*$', resolved, re.M)
        self.assertIn(f'dist/cwist-{version}.tar.gz', paths)
        for name in ('SHA256SUMS', 'SOURCE_COMMIT', 'SOURCE_TREE', 'SUBMODULES'):
            self.assertIn(f'dist/{name}', paths)

    def test_native_kqueue_job_runs_cache_unit_and_tcp_contract(self):
        text = (ROOT / '.github/workflows/bsd-kqueue-benchmarks.yml').read_text()
        job = text.split('  bsd-kqueue:\n', 1)[1].split('\n  benchmark:', 1)[0]
        commands = '\n'.join(line.strip() for line in job.splitlines()
                             if line.strip().startswith('make '))
        for target in ('test_public_fixed_cache', 'test_public_fixed_http'):
            self.assertRegex(commands, rf'\b{target}\b')


if __name__ == '__main__':
    unittest.main()
