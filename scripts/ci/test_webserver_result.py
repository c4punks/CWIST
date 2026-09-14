import json,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from webserver_result import parse_wrk_text,validate_case,summarize_resources,validate_warmup_text,build_result,CASES,append_history,version_text

def sample():
 return {'requests':200000,'duration_us':10000000,'errors':dict.fromkeys(['connect','read','write','timeout','status'],0),'mean_us':200,'min_us':10,'max_us':9000,'percentiles_us':dict(zip(['50','75','90','99','99.9','99.99','99.999'],[100,200,300,1000,2000,4000,8000]))}
def output(x):return 'CWIST_METRICS '+json.dumps(x)+'\n'
class MetricsTests(unittest.TestCase):
 def test_version_probe(self):
  self.assertEqual(version_text(1,'wrk 4.2.0 [epoll]\nUsage: wrk ...'),'wrk 4.2.0 [epoll]')
  for code,text in [(2,'wrk 4.2.0'),(1,'error'),(0,'')]:
   with self.assertRaises(ValueError):version_text(code,text)
 def test_warmup(self):
  validate_warmup_text('200 requests in 10.0s, 1MB read')
  for s in ['', '0 requests in 10s', '200 requests in 10s\nNon-2xx or 3xx responses: 200', '200 requests in 10s\nSocket errors: connect 1, read 0, write 0, timeout 0']:
   with self.subTest(s=s),self.assertRaises(ValueError):validate_warmup_text(s)
 def test_units(self):
  x=parse_wrk_text(output(sample()));self.assertEqual((x['rps'],x['lat_ms'],x['p99_999'],x['requests']),(20000,.2,8,200000))
 def test_missing_duplicate(self):
  for s in ['', 'Requests/sec: 123',output(sample())*2,'CWIST_METRICS not-json']:
   with self.subTest(s=s),self.assertRaises(ValueError):parse_wrk_text(s)
 def test_errors(self):
  for k in sample()['errors']:
   x=sample();x['errors'][k]=1
   with self.subTest(k=k),self.assertRaises(ValueError):parse_wrk_text(output(x))
 def test_invalid_numbers(self):
  for k,v in [('requests',0),('requests',True),('requests',1.5),('duration_us',0),('mean_us',float('nan')),('max_us',float('inf')),('min_us',-1)]:
   x=sample();x[k]=v
   with self.subTest(k=k,v=v),self.assertRaises(ValueError):parse_wrk_text(output(x))
 def test_missing_counter_and_percentile_order(self):
  x=sample();del x['errors']['status']
  with self.assertRaises(ValueError):parse_wrk_text(output(x))
  x=sample();x['percentiles_us']['99.99']=9001
  with self.assertRaises(ValueError):parse_wrk_text(output(x))
 def test_cleanup_and_readiness(self):
  m=parse_wrk_text(output(sample()));r={'complete':True,'cleanup_ok':True,'survivors':[]};t={'ready':True,'warmup_ok':True,'measurement_exit':0}
  validate_case(m,r,t)
  for k in ['complete','cleanup_ok']:
   with self.subTest(k=k),self.assertRaises(ValueError):validate_case(m,dict(r,**{k:False}),t)
  for k,v in [('ready',False),('warmup_ok',False),('measurement_exit',1)]:
   with self.subTest(k=k),self.assertRaises(ValueError):validate_case(m,r,dict(t,**{k:v}))
  with self.assertRaises(ValueError):validate_case(m,dict(r,survivors=[12]),t)
 def test_resources(self):
  def p(pid,start,rss,csw):return {'pid':pid,'start':start,'rss_kib':rss,'tasks':[{'tid':pid,'start':start,'csw':csw}]}
  b=[p(1,'10',20,5),p(2,'20',30,10)];a=[p(1,'10',22,8),p(2,'20',33,17)]
  self.assertEqual(summarize_resources(b,a),{'rss_kib':55,'csw':10,'rss_kind':'process-group end sample','csw_kind':'same-TID counter delta'})
  a[1]['start']='21';self.assertIsNone(summarize_resources(b,a)['csw'])
  a[1]['rss_kib']=None;self.assertIsNone(summarize_resources(b,a)['rss_kib'])
class AggregateTests(unittest.TestCase):
 def cases(self):
  return {key:(parse_wrk_text(output(sample())),{'complete':True,'cleanup_ok':True,'survivors':[]},{'before':[],'after':[],'ready':True,'warmup_ok':True,'measurement_exit':0}) for key in CASES}
 def meta(self):return {'commit':'a'*40,'run_id':'12','run_attempt':'1','binary_sha256':{'cwist':'b'*64},'wrk_version':'wrk 4.2.0'}
 def test_bound_complete_matrix(self):
  value=build_result(self.cases(),self.meta());self.assertEqual(value['schema_version'],2);self.assertEqual(len(value['measurements']),10);self.assertEqual(value['cwist_p99_999_ms'],8)
 def test_history_binding(self):
  row=build_result(self.cases(),self.meta());old=[{'cwist_rps':0}]
  result=append_history(old,row,'12','a'*40);self.assertEqual(result[:-1],old);self.assertEqual(old,[{'cwist_rps':0}])
  self.assertEqual(append_history(result,row,'12','a'*40),result)
  with self.assertRaises(ValueError):append_history(old,row,'13','a'*40)
 def test_missing_or_extra_case(self):
  for mode in ('missing','extra'):
   data=self.cases()
   if mode=='missing':data.pop('spring')
   else:data['unknown']=data['cwist']
   with self.assertRaises(ValueError):build_result(data,self.meta())
 def test_missing_source_or_bad_sha(self):
  for meta in ({},dict(self.meta(),commit='not-sha')):
   with self.assertRaises(ValueError):build_result(self.cases(),meta)
if __name__=='__main__':unittest.main()
