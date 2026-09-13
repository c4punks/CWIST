"""Functional workload order and artifact-whitelist tests; no real load."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT=Path(__file__).resolve().parents[2]
SCRIPT=ROOT/'scripts/ci/benchmark_workload.sh'

class WorkloadTelemetryTests(unittest.TestCase):
    def test_snapshots_bound_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); tools=root/'bin'; tools.mkdir()
            scripts=root/'scripts/ci'; scripts.mkdir(parents=True)
            events=root/'events.jsonl'; output=root/'case.txt'
            for name in ('ps','curl'):
                p=tools/name; p.write_text('#!/bin/sh\nprintf "0\\n"\n'); p.chmod(0o755)
            fake="import json,os,sys\nwith open(os.environ['EVENTS'],'a') as f:f.write(json.dumps(['measure' if '--latency' in sys.argv else 'warm',sys.argv[1:]])+'\\n')\nprint('stub wrk')\n"
            p=tools/'wrk'; p.write_text('#!'+sys.executable+'\n'+fake); p.chmod(0o755)
            (scripts/'benchmark_probe.py').write_text("import json,os,sys\nwith open(os.environ['EVENTS'],'a') as f:f.write(json.dumps([sys.argv[1],sys.argv[2:]])+'\\n')\nprint(json.dumps({'complete':False,'context_switch_delta':None}))\n")
            env=os.environ.copy();env.update(PATH=str(tools)+os.pathsep+env['PATH'],GITHUB_WORKSPACE=str(root),BENCHMARK_SERVER_PID='123',EVENTS=str(events))
            result=subprocess.run(['bash',str(SCRIPT),'3000',str(output),str(root/'stat.txt'),'1','4','100'],env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            rows=[json.loads(line) for line in events.read_text().splitlines()]
            self.assertEqual([r[0] for r in rows],['warm','snapshot','measure','snapshot','summarize'])
            self.assertEqual(rows[0][1][:3],['-t4','-c100','-d10s'])
            self.assertEqual(rows[2][1][:3],['-t4','-c100','-d10s'])
            self.assertEqual(rows[1][1],['--pgid','123'])
            self.assertEqual(rows[3][1],['--pgid','123'])
            self.assertEqual(rows[4][1],[str(output)+'.proc-before.json',str(output)+'.proc-after.json'])
            for phase in ('before','after','summary'):
                data=json.loads(Path(str(output)+'.proc-'+phase+'.json').read_text())
                self.assertIsNone(data['context_switch_delta'])

    def test_artifact_whitelist(self):
        text=(ROOT/'.github/workflows/bsd-kqueue-benchmarks.yml').read_text()
        block=text.split('- name: Copy raw benchmark output\n',1)[1].split('\n      - uses:',1)[0]
        command=textwrap.dedent(block.split('run: |\n',1)[1])
        profiles=('cwist','cwist_c1m','cwist_c1m_arena1','cwist_tuned','axum','gin','spring','spring_tuned')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); source=root/'inputs';source.mkdir()
            expected={f'{name}.txt.proc-{phase}.json' for name in profiles for phase in ('before','after','summary')}
            for name in expected | {'unrelated.txt.proc-before.json'}:(source/name).write_text('{}')
            result=subprocess.run(['bash','-e','-c',command.replace('/tmp/',str(source)+'/')],cwd=root,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual({p.name for p in (root/'raw_logs').iterdir()},expected|{'proc-telemetry-metadata.json'})
            self.assertEqual(len(expected),24)

    def test_failure_artifacts_mark_legacy_zeros_untrusted(self):
        workflow=SCRIPT.parents[2]/'.github/workflows/bsd-kqueue-benchmarks.yml'
        block=workflow.read_text().split('- name: Copy raw benchmark output',1)[1].split('\n      - uses:',1)[0]
        shell=textwrap.dedent(block.split('run: |\n',1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); inputs=root/'inputs'; inputs.mkdir()
            (inputs/'cwist_stat.txt').write_text('rss_kib=100 csw=0\n')
            subprocess.run(['bash','-e','-c',shell.replace('/tmp/',str(inputs)+'/')],cwd=root,check=True,timeout=10)
            output=root/'raw_logs'
            self.assertTrue((output/'cwist_stat.txt').is_file())
            self.assertFalse((root/'webserver-result.json').exists())
            metadata=json.loads((output/'proc-telemetry-metadata.json').read_text())
            self.assertIs(metadata['legacy_csw_reliable'],False)

if __name__=='__main__': unittest.main()
