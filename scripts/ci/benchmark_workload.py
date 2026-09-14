"""Identity-bound Linux snapshots for a supervised benchmark process group."""
import os
from pathlib import Path


def snapshot_group(pgid, proc=Path('/proc')):
    rows = []
    for path in sorted(proc.iterdir(), key=lambda p: int(p.name) if p.name.isdigit() else -1):
        if not path.name.isdigit():
            continue
        try:
            fields = (path / 'stat').read_text().rsplit(') ', 1)[1].split()
            if int(fields[2]) != pgid or fields[0] in ('Z', 'X'):
                continue
            status = dict(line.split(':', 1) for line in (path / 'status').read_text().splitlines() if ':' in line)
            tasks = []; tasks_complete = True
            for task in sorted((path / 'task').iterdir()):
                try:
                    stat = (task / 'stat').read_text().rsplit(') ', 1)[1].split()
                    values = dict(line.split(':', 1) for line in (task / 'status').read_text().splitlines() if ':' in line)
                    counts = [values.get(k) for k in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches')]
                    csw = sum(int(v) for v in counts if v is not None) if all(v is not None for v in counts) else None
                    tasks.append({'tid': int(task.name), 'start': stat[19], 'csw': csw,
                                  'cpus': values.get('Cpus_allowed_list', '').strip()})
                except (FileNotFoundError, ProcessLookupError):
                    tasks_complete = False
                    continue
            fds = {}
            try:
                for fd in (path / 'fd').iterdir():
                    try:
                        target = os.readlink(fd)
                        if target.startswith('anon_inode:'):
                            fds[target] = fds.get(target, 0) + 1
                    except (FileNotFoundError, ProcessLookupError):
                        pass
            except PermissionError:
                fds = None
            rss = status.get('VmRSS')
            rows.append({'pid': int(path.name), 'start': fields[19], 'tasks': tasks, 'tasks_complete': tasks_complete,
                         'cpus': status.get('Cpus_allowed_list', '').strip(),
                         'rss_kib': int(rss.split()[0]) if rss is not None else None,
                         'nofile': [line for line in (path / 'limits').read_text().splitlines()
                                    if line.startswith('Max open files')], 'fds': fds})
        except (FileNotFoundError, ProcessLookupError):
            continue
    if not rows:
        raise RuntimeError('supervised server group disappeared')
    return rows


def main(argv=None):
    import argparse
    import http.client
    import json
    import subprocess
    import time
    from webserver_result import parse_wrk_text, validate_warmup_text
    parser = argparse.ArgumentParser()
    parser.add_argument('port', type=int)
    parser.add_argument('text', type=Path)
    parser.add_argument('stats', type=Path)
    parser.add_argument('wait', type=int)
    parser.add_argument('threads', type=int)
    parser.add_argument('connections', type=int)
    args = parser.parse_args(argv)
    if 'BENCHMARK_SERVER_PID' not in os.environ:
        parser.error('BENCHMARK_SERVER_PID is required')
    if not 1 <= args.port <= 65535 or not 1 <= args.wait <= 120 or min(args.threads, args.connections) <= 0:
        parser.error('invalid workload bounds')
    pgid = int(os.environ['BENCHMARK_SERVER_PID'])
    telemetry = {'schema': 2, 'server_pgid': pgid, 'complete': False,
                 'ready': False, 'warmup_ok': False, 'measurement_exit': None}
    url = f'http://127.0.0.1:{args.port}/'
    lua = Path(__file__).with_name('tail_latency.lua')
    base = ['wrk', f'-t{args.threads}', f'-c{args.connections}', '-d10s']
    try:
        deadline = time.monotonic() + args.wait
        while True:
            snapshot_group(pgid)
            connection = http.client.HTTPConnection('127.0.0.1', args.port, timeout=1)
            ready = False
            try:
                connection.request('GET', '/')
                response = connection.getresponse()
                response.read()
                ready = response.status == 200
            except (OSError, http.client.HTTPException):
                pass
            finally:
                connection.close()
            if ready:
                telemetry['ready'] = True
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('server readiness deadline exceeded')
            time.sleep(.1)
        warm = base + [url]
        with args.text.with_suffix('.warm.log').open('w') as output:
            subprocess.run(warm, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=45)
        validate_warmup_text(args.text.with_suffix('.warm.log').read_text())
        telemetry['warmup_ok'] = True
        telemetry['before'] = snapshot_group(pgid)
        measured = base + ['-s', str(lua), '--latency', url]
        telemetry.update({'warm_command': warm, 'command': measured})
        start = time.monotonic()
        with args.text.open('w') as output:
            completed = subprocess.run(measured, stdout=output, stderr=subprocess.STDOUT, timeout=45)
        telemetry['measurement_exit'] = completed.returncode
        completed.check_returncode()
        telemetry['elapsed_seconds'] = time.monotonic() - start
        telemetry['after'] = snapshot_group(pgid)
        identity = lambda rows: {(p['pid'], p['start']) for p in rows}
        if identity(telemetry['before']) != identity(telemetry['after']):
            raise RuntimeError('server process identity changed during measurement')
        parse_wrk_text(args.text.read_text())
        telemetry['complete'] = True
    except BaseException as error:
        telemetry['failure'] = type(error).__name__
        raise
    finally:
        args.stats.write_text(json.dumps(telemetry, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
