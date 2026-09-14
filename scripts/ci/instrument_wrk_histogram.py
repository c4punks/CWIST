#!/usr/bin/env python3
"""Generate opt-in wrk 4.2.0 reporting files; never modify the input checkout."""
import argparse
import hashlib
import json
from pathlib import Path

KNOWN = {
    'wrk.c': {'35598eb21cd21d0b4247ae1d7acbf4492f8cbca92f414ddab0fdfada996867a9',
              '86e49a4dcd92622934bf0e0b65439ee412587fe0f7e4a7eeb0e2e9730583be41'},
    'stats.c': {'1f954e1f22a3a97a061e743e4cbacf801358d63427fec1ee5c13b315b0b40e03'},
    'stats.h': {'17fbe4a7410effd7de813decd7468362d2e109d27e0fa01210500c46491a9435'},
}
CORRECTION = '''    if (complete / cfg.connections > 0) {
        int64_t interval = runtime_us / (complete / cfg.connections);
        stats_correct(statistics.latency, interval);
    }
'''


def instrument_text(source):
    if 'cwist_dump_histogram' in source or 'wrk_dual_histogram.h' in source:
        raise ValueError('Already instrumented')
    if source.count(CORRECTION) != 1 or source.count('#include "wrk.h"\n') != 1:
        raise ValueError('Unexpected reporting layout')
    if not (source.index('pthread_join(') < source.index('runtime_us = time_us() - start;') < source.index(CORRECTION)):
        raise ValueError('Reporting must follow worker joins and frozen runtime')
    return source.replace('#include "wrk.h"\n', '#include "wrk.h"\n#include "wrk_dual_histogram.h"\n').replace(
        CORRECTION, '    cwist_dump_histogram("raw", statistics.latency);\n' + CORRECTION +
        '    cwist_dump_histogram("corrected", statistics.latency);\n')


def generate(source_dir, output_dir):
    inputs = {}
    validated = {}
    for name, allowed in KNOWN.items():
        data = (source_dir / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest not in allowed:
            raise ValueError('Unrecognized wrk 4.2.0 source: ' + name)
        inputs[name] = digest
        validated[name] = data
    generated = instrument_text(validated['wrk.c'].decode('utf-8')).encode()
    header = Path(__file__).with_name('wrk_dual_histogram.h').read_bytes()
    output_dir.mkdir(parents=False, exist_ok=False)
    for name, data in [('wrk.c', generated), ('wrk_dual_histogram.h', header)]:
        (output_dir / name).write_bytes(data)
    record = {'inputs': inputs, 'outputs': {'wrk.c': hashlib.sha256(generated).hexdigest(),
              'wrk_dual_histogram.h': hashlib.sha256(header).hexdigest()}}
    (output_dir / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', required=True, type=Path, help='wrk checkout src directory (read-only)')
    parser.add_argument('--output-dir', required=True, type=Path, help='new directory under an existing parent')
    args = parser.parse_args()
    print(json.dumps(generate(args.source_dir, args.output_dir), indent=2))


if __name__ == '__main__':
    main()
