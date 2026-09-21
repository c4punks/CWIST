"""Strict wrk result admission; unavailable telemetry is never a zero sample."""
import json
import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runner_baseline import evaluate


def validate_warmup_text(text):
    match = re.search(r'\b(\d+) requests in ', text)
    require(match is not None and int(match.group(1)) > 0, 'empty warmup')
    for line in text.splitlines():
        if 'Socket errors:' in line or 'Non-2xx or 3xx responses:' in line:
            require(not any(int(n) for n in re.findall(r'\b\d+\b', line)), 'warmup request errors')

PERCENTILES = ('50', '75', '90', '99', '99.9', '99.99', '99.999')
ERRORS = ('connect', 'read', 'write', 'timeout', 'status')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, *, integer=False):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            'nonfinite, negative or invalid numeric value')
    if integer:
        require(type(value) is int, 'counter must be an integer')
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def parse_wrk_text(text):
    lines = [line[len('CWIST_METRICS '):] for line in text.splitlines()
             if line.startswith('CWIST_METRICS ')]
    require(len(lines) == 1, 'exactly one structured wrk result required')
    try:
        data = json.loads(lines[0], object_pairs_hook=unique_object)
        requests = number(data['requests'], integer=True)
        duration = number(data['duration_us'])
        require(requests > 0 and duration > 0, 'empty measurement')
        errors = data['errors']
        require(set(errors) == set(ERRORS), 'missing error counters')
        require(all(number(errors[k], integer=True) == 0 for k in ERRORS),
                'wrk reported request errors')
        low, mean, high = [number(data[k]) for k in ('min_us', 'mean_us', 'max_us')]
        q = data['percentiles_us']
        require(set(q) == set(PERCENTILES), 'missing percentiles')
        values = [low] + [number(q[k]) for k in PERCENTILES] + [high]
        require(values == sorted(values) and low <= mean <= high,
                'inconsistent latency distribution')
    except (KeyError, TypeError, AttributeError, json.JSONDecodeError) as error:
        raise ValueError('malformed wrk result') from error
    result = {'requests': requests, 'duration_us': duration, 'errors': errors,
              'rps': requests / (duration / 1e6), 'lat_ms': mean / 1000,
              'min_ms': low / 1000, 'max_ms': high / 1000,
              'latency_kind': 'wrk corrected latency distribution',
              'expected_tail_observations': requests * 0.00001}
    for key, output in zip(PERCENTILES, ('p50', 'p75', 'p90', 'p99', 'p999', 'p9999', 'p99_999')):
        result[output] = q[key] / 1000
    return result


def validate_case(metrics, receipt, telemetry):
    require(receipt.get('complete') is True and receipt.get('cleanup_ok') is True
            and receipt.get('survivors') == [], 'cleanup not proven')
    require(telemetry.get('ready') is True and telemetry.get('warmup_ok') is True
            and type(telemetry.get('measurement_exit')) is int
            and telemetry['measurement_exit'] == 0, 'workload did not succeed')
    require(metrics.get('requests', 0) > 0, 'empty result')


def summarize_resources(before, after):
    rss = None
    if after and all(type(p.get('rss_kib')) is int and p['rss_kib'] >= 0 for p in after):
        rss = sum(p['rss_kib'] for p in after)
    # pss_kib (issue #150): summing rss_kib across a multi-process server's
    # workers counts each page shared between them (the binary's own .text,
    # shared libraries) once per process, overstating the group's real
    # memory use relative to a single-process competitor whose rss_kib
    # already reflects reality. Pss divides a shared page's cost by however
    # many processes map it, so summing pss_kib across the group gives its
    # actual unique footprint. Only available when snapshot_group() could
    # read smaps_rollup for every process.
    pss = None
    if after and all(type(p.get('pss_kib')) is int and p['pss_kib'] >= 0 for p in after):
        pss = sum(p['pss_kib'] for p in after)
    def counters(rows):
        values = {}
        for process in rows:
            require(process.get('tasks_complete', True), 'task snapshot changed')
            for task in process['tasks']:
                key = (process['pid'], process['start'], task['tid'], task['start'])
                require(key not in values, 'duplicate task identity')
                values[key] = task.get('csw')
        return values
    csw = None
    try:
        first, last = counters(before), counters(after)
        if first and first.keys() == last.keys() and all(
                type(first[k]) is int and type(last[k]) is int and last[k] >= first[k]
                for k in first):
            csw = sum(last[k] - first[k] for k in first)
    except (KeyError, TypeError, ValueError):
        pass
    return {'rss_kib': rss, 'pss_kib': pss, 'csw': csw, 'rss_kind': 'process-group end sample',
            'pss_kind': 'process-group end sample, shared pages divided by mapper count'
                        if pss is not None else 'unavailable: smaps_rollup unreadable',
            'csw_kind': 'same-TID counter delta' if csw is not None else 'unavailable: task churn or missing counters'}


CASES = ('cwist', 'cwist_c1m', 'cwist_c1m_arena1', 'cwist_c1m_drainchunk',
         'cwist_tuned', 'axum', 'axum_tuned', 'gin', 'spring', 'spring_tuned')


def build_result(cases, metadata, history=None):
    require(set(cases) == set(CASES), 'incomplete or unknown benchmark matrix')
    require(re.fullmatch(r'[0-9a-f]{40}', metadata.get('commit', '')) is not None, 'source commit')
    require(metadata.get('run_id') and metadata.get('run_attempt') and metadata.get('binary_sha256') and metadata.get('wrk_version'), 'source provenance')
    result = dict(metadata, schema_version=2, benchmark_contract='isolated-http1-wrk-corrected-v2',
                  latency_kind='wrk corrected distribution', measurements={})
    keys = {'50':'p50', '75':'p75', '90':'p90', '99':'p99', '99.9':'p999', '99.99':'p9999', '99.999':'p99_999'}
    for name in CASES:
        raw, receipt, telemetry = cases[name]
        validate_case(raw, receipt, telemetry)
        resources = summarize_resources(telemetry['before'], telemetry['after'])
        for target in ['rps', 'lat_ms', 'min_ms', 'max_ms']:
            result[name+'_'+target] = raw[target]
        for target in keys.values():
            result[name+'_'+target+'_ms'] = raw[target]
        result[name+'_rss_kib'] = resources['rss_kib']
        result[name+'_pss_kib'] = resources['pss_kib']
        result[name+'_csw'] = resources['csw']
        result['measurements'][name] = {'requests':raw['requests'], 'errors':raw['errors'],
            'duration_us':raw['duration_us'], 'resource_unavailable':{'rss': 'missing counters' if resources['rss_kib'] is None else None, 'pss': resources['pss_kind'] if resources['pss_kib'] is None else None, 'csw': resources['csw_kind'] if resources['csw'] is None else None},
            'expected_count_above_p99_999':raw['requests'] * .00001,
            'profile':'tuned' if name.endswith('_tuned') else 'main',
            'server_pgid':telemetry.get('server_pgid'), 'before':telemetry['before'], 'after':telemetry['after'],
            'command':telemetry.get('command'), 'cleanup':receipt}
    # Latency gate. A single absolute number is a lottery here: the runner
    # CPU model changes run to run and moves these numbers more than most
    # code changes do, so the same build sits at 87% of a 3.5ms limit on one
    # CPU and 54% on another. Compare against earlier runs on the same CPU
    # when that history is available, keeping the absolute number as the
    # backstop. Allowances are the measured p99 of each metric's own
    # run-to-run noise; see runner_baseline.py.
    for name, ceiling, allowance in [('cwist', 3.0, 1.25), ('cwist_c1m', 3.5, 1.55)]:
        metric = name + '_lat_ms'
        ok, detail = evaluate(history or [], result, metric, ceiling,
                              allowance=allowance)
        require(ok, detail)
    return result


def append_history(history, row, run_id, commit):
    require(row.get('schema_version') == 2 and row.get('benchmark_contract') == 'isolated-http1-wrk-corrected-v2', 'benchmark contract')
    require(row.get('run_id') == run_id and row.get('commit') == commit, 'source/run binding')
    require(set(row.get('measurements', {})) == set(CASES), 'incomplete published matrix')
    for value in row['measurements'].values():
        require(number(value['requests'], integer=True) > 0, 'empty published measurement')
        require(set(value['errors']) == set(ERRORS) and all(number(v, integer=True) == 0 for v in value['errors'].values()), 'published request errors')
        require(value['cleanup'].get('complete') is True and value['cleanup'].get('cleanup_ok') is True and value['cleanup'].get('survivors') == [], 'published cleanup')
    key = lambda value: (value.get('run_id'), value.get('run_attempt'), value.get('benchmark_contract'))
    for old in history:
        if key(old) == key(row):
            require(old == row, 'conflicting result for one run')
            return list(history)
    return (list(history) + [row])[-100:]


def version_text(code, text):
    first = text.splitlines()[0] if text.splitlines() else ''
    require(code in (0, 1) and re.match(r'^wrk \S+', first), 'wrk version probe failed')
    return first


def main():
    import argparse
    import hashlib
    import os
    import shutil
    import subprocess
    from datetime import datetime, timezone
    from pathlib import Path
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path('/tmp'))
    parser.add_argument('--output', type=Path, default=Path('webserver-result.json'))
    args = parser.parse_args()
    def command(*parts):
        return subprocess.check_output(parts, text=True, timeout=20).strip()
    commit = command('git','rev-parse','HEAD')
    require(commit == os.environ.get('GITHUB_SHA'), 'checkout/GITHUB_SHA mismatch')
    require(not command('git','diff','--name-only','HEAD'), 'modified tracked measurement source')
    binaries = {'cwist':args.root/'cwist_server', 'axum':args.root/'axum_bench/target/release/axum_server',
                'gin':args.root/'gin_bench/gin_server', 'spring':args.root/'spring_bench/target/spring-bench-0.0.1-SNAPSHOT.jar',
                'wrk':Path(shutil.which('wrk') or '/nonexistent-wrk')}
    version = subprocess.run(['wrk', '--version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
    metadata = {'timestamp':datetime.now(timezone.utc).isoformat(), 'commit':commit,
        'run_id':os.environ['GITHUB_RUN_ID'], 'run_attempt':os.environ['GITHUB_RUN_ATTEMPT'],
        'run_url':'https://github.com/'+os.environ['GITHUB_REPOSITORY']+'/actions/runs/'+os.environ['GITHUB_RUN_ID'],
        'ref':os.environ['GITHUB_REF'], 'release_tag':os.environ['GITHUB_REF'].removeprefix('refs/tags/') if os.environ['GITHUB_REF'].startswith('refs/tags/') else None,
        'binary_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in binaries.items()},
        'wrk_version':version_text(version.returncode, version.stdout), 'wrk_profile':os.environ['WRK_PROFILE'],
        'tuned_profile':os.environ['TUNED_PROFILE'], 'runner_hw':os.environ['RUNNER_HW'],
        'go_env':{'go_version':os.environ['GO_VERSION_RECORDED'], 'framework':os.environ['GIN_FRAMEWORK']},
        'spring_env':{'java_version':os.environ['SPRING_JAVA_VERSION'], 'spring_boot_version':os.environ['SPRING_BOOT_VERSION'],
            'stack':os.environ['SPRING_STACK'], 'jvm_opts':os.environ['SPRING_JVM_OPTS_RECORDED'],
            'virtual_threads':False, 'aot_cache':'JDK 25 Leyden AOT (-XX:AOTCache; trained before measurement)'}}
    cases = {}
    for name in CASES:
        raw = parse_wrk_text((args.root/(name+'.txt')).read_text())
        receipt = json.loads((args.root/(name+'.session.json')).read_text())
        telemetry = json.loads((args.root/(name+'_stat.txt')).read_text())
        require(telemetry.get('complete') is True, 'workload incomplete')
        require(type(telemetry.get('server_pgid')) is int and telemetry['server_pgid']==receipt.get('server_pgid'), 'group binding')
        require(telemetry['before'] and telemetry['after'], 'missing topology')
        cases[name] = raw,receipt,telemetry
    history_path = args.root.parent / 'benchmarks' / 'webserver.json'
    if not history_path.exists():
        history_path = Path('benchmarks/webserver.json')
    try:
        history = json.loads(history_path.read_text())
    except (OSError, ValueError):
        history = []
    # Only rows measured under the current contract are comparable; a row
    # from an older measurement definition is as misleading a baseline as a
    # row from a different CPU.
    history = [r for r in history
               if isinstance(r, dict)
               and r.get('benchmark_contract') == 'isolated-http1-wrk-corrected-v2']
    result = build_result(cases,metadata,history)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print('Accepted',len(cases),'isolated cases;',sum(x[0]['requests'] for x in cases.values()),'requests; reported errors 0')


if __name__ == '__main__':
    main()
