"""Offline contract checks for the opt-in wrk post-run histogram exporter."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

def load_tool():
    spec = importlib.util.spec_from_file_location('instrument', HERE/'instrument_wrk_histogram.py')
    if spec is None or spec.loader is None:
        raise RuntimeError('Cannot load instrumentation module')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

class HistogramTests(unittest.TestCase):
    def test_sparse_empty_and_below_original_minimum(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'stats.h').write_text('#include <stdint.h>\ntypedef struct { uint64_t count,limit,min,max; uint64_t data[64]; } stats;\n')
            (p/'check.c').write_text('''#include "wrk_dual_histogram.h"
int main(void) {
    stats s = {.count=2,.limit=64,.min=40,.max=40};
    s.data[40]=2;
    cwist_dump_histogram("raw", &s);
    s.count=6; s.data[20]=2; s.data[30]=2;
    cwist_dump_histogram("corrected", &s);
    stats empty = {.limit=64,.min=UINT64_MAX};
    cwist_dump_histogram("empty", &empty);
    return 0;
}
''')
            result = subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(HERE),str(p/'check.c'),'-o',str(p/'check')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            output = subprocess.check_output([str(p/'check')],text=True)
            records = [json.loads(line.removeprefix('CWIST_HISTOGRAM ')) for line in output.splitlines()]
            self.assertEqual(records,[
                {'kind':'raw','count':2,'unit':'us','bins':[[40,2]]},
                {'kind':'corrected','count':6,'unit':'us','bins':[[20,2],[30,2],[40,2]]},
                {'kind':'empty','count':0,'unit':'us','bins':[]},
            ])

    def test_patch_only_brackets_correction_after_timing(self):
        mod = load_tool()
        source = '#include "wrk.h"\n/* timed request loop */\npthread_join(t->thread, NULL);\nuint64_t runtime_us = time_us() - start;\n' + mod.CORRECTION + '\n/* remaining report */\n'
        result = mod.instrument_text(source)
        self.assertEqual(result.replace('#include "wrk_dual_histogram.h"\n','').replace('    cwist_dump_histogram("raw", statistics.latency);\n','').replace('    cwist_dump_histogram("corrected", statistics.latency);\n',''),source)
        self.assertLess(result.index('runtime_us ='),result.index('cwist_dump_histogram("raw"'))
        self.assertLess(result.index('cwist_dump_histogram("raw"'),result.index('stats_correct('))
        self.assertLess(result.index('stats_correct('),result.index('cwist_dump_histogram("corrected"'))
        for invalid in [source.replace(mod.CORRECTION,''),source+mod.CORRECTION,result]:
            with self.assertRaises(ValueError): mod.instrument_text(invalid)

    def test_generation_uses_the_fingerprinted_bytes(self):
        from unittest import mock
        import hashlib
        mod = load_tool()
        original = ('#include "wrk.h"\npthread_join(t->thread, NULL);\n'
                    'uint64_t runtime_us = time_us() - start;\n' + mod.CORRECTION).encode()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root/'src'
            src.mkdir()
            fixture = {'wrk.c': original, 'stats.c': b'stats fixture', 'stats.h': b'header fixture'}
            for n, data in fixture.items():
                (src/n).write_bytes(data)
            allowed = {n: {hashlib.sha256(data).hexdigest()} for n, data in fixture.items()}
            real_read = Path.read_bytes
            reads = []
            def edit_after_read(path):
                data = real_read(path)
                if path == src/'wrk.c':
                    reads.append(path)
                    path.write_bytes(original + b'/* unrecognized concurrent edit */\n')
                return data
            with mock.patch.dict(mod.KNOWN, allowed, clear=True), mock.patch.object(Path, 'read_bytes', edit_after_read):
                manifest = mod.generate(src, root/'output')
            self.assertEqual(len(reads), 1)
            self.assertEqual((root/'output/wrk.c').read_bytes(), mod.instrument_text(original.decode()).encode())
            self.assertEqual(manifest['inputs']['wrk.c'], hashlib.sha256(original).hexdigest())
            (src/'wrk.c').write_bytes(original)
            before = {p.name: p.read_bytes() for p in (root/'output').iterdir()}
            with mock.patch.dict(mod.KNOWN, allowed, clear=True), self.assertRaises(FileExistsError):
                mod.generate(src, root/'output')
            self.assertEqual({p.name: p.read_bytes() for p in (root/'output').iterdir()}, before)

    def test_unknown_source_is_rejected_without_writes(self):
        mod = load_tool()
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            src = p/'src'
            src.mkdir()
            (src/'wrk.c').write_text('unsupported source\n')
            out = p/'out'
            out.mkdir()
            (out/'sentinel').write_text('preserve')
            with self.assertRaises(ValueError): mod.generate(src,out)
            self.assertEqual((src/'wrk.c').read_text(),'unsupported source\n')
            self.assertEqual(list(out.iterdir()),[out/'sentinel'])
            self.assertEqual((out/'sentinel').read_text(),'preserve')

if __name__ == '__main__':
    unittest.main()
