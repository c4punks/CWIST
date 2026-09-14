import tempfile,unittest,sys,subprocess,os
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from benchmark_workload import snapshot_group
class SnapshotTests(unittest.TestCase):
 def test_group_only_and_task_counters(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)
   for pid,group in [(100,100),(101,100),(102,999)]:
    p=root/str(pid);(p/'task'/str(pid)).mkdir(parents=True)
    fields=['S','1',str(group)]+['0']*16+['123']
    stat=f'{pid} (worker) '+' '.join(fields)
    (p/'stat').write_text(stat);(p/'task'/str(pid)/'stat').write_text(stat)
    status='VmRSS: 10 kB\nCpus_allowed_list: 0-1\nvoluntary_ctxt_switches: 3\nnonvoluntary_ctxt_switches: 4\n'
    (p/'status').write_text(status);(p/'task'/str(pid)/'status').write_text(status)
    (p/'limits').write_text('Max open files            1024 1024 files\n');(p/'fd').mkdir()
   x=snapshot_group(100,root)
   self.assertEqual([p['pid'] for p in x],[100,101]);self.assertEqual(x[0]['tasks'][0]['csw'],7);self.assertEqual(x[0]['rss_kib'],10)
   (root/'100/task/100/status').write_text('Cpus_allowed_list: 0-1\n')
   self.assertIsNone(snapshot_group(100,root)[0]['tasks'][0]['csw'])
   (root/'100/task/100/status').unlink()
   changed=snapshot_group(100,root)
   self.assertEqual(len(changed),2);self.assertFalse(changed[0]['tasks_complete'])
 def test_empty_group_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(RuntimeError):snapshot_group(1,Path(d))
class WorkloadCliTests(unittest.TestCase):
 def test_requires_supervised_server(self):
  env=dict(os.environ);env.pop('BENCHMARK_SERVER_PID',None)
  with tempfile.TemporaryDirectory() as d:
   p=subprocess.run([sys.executable,str(Path(__file__).with_name('benchmark_workload.py')),'9091',d+'/load.txt',d+'/stats.json','1','1','1'],env=env,capture_output=True,text=True)
   self.assertNotEqual(p.returncode,0);self.assertIn('BENCHMARK_SERVER_PID',p.stderr)
if __name__=='__main__':unittest.main()
