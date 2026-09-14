import unittest
from benchmark import webserver_summary,compatible_history,render_webserver_svg
class PresentationTests(unittest.TestCase):
 def row(self):return {'schema_version':2,'benchmark_contract':'isolated-http1-wrk-corrected-v2','commit':'a'*40,'run_url':'https://github.com/c4punks/CWIST/actions/runs/123','timestamp':'2026-09-14','cwist_rps':1234.5,'cwist_lat_ms':1.25,'cwist_p99_999_ms':9.75,'cwist_rss_kib':None,'cwist_csw':None}
 def test_provenance_units_missing_resources(self):
  text=webserver_summary(self.row());self.assertIn('a'*40,text);self.assertIn('runs/123',text);self.assertIn('N/A',text);self.assertIn('corrected',text);self.assertIn('9.750',text)
 def test_no_contract_mixing(self):
  old={'cwist_rps':999999};new=self.row();self.assertEqual(compatible_history([old,new]),[new]);self.assertEqual(compatible_history([old]),[old])
 def test_svg_marks_unavailable(self):
  svg=render_webserver_svg([self.row()]);self.assertIn('N/A',svg);self.assertNotIn('Peak RSS',svg)
 def test_resource_sample_not_peak(self):
  text=webserver_summary(self.row());self.assertIn('end sample',text);self.assertNotIn('Peak RSS',text)
if __name__=='__main__':unittest.main()
