"""Zero-I/O unit tests for exact stock-tree admission before codec compilation."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

runner=Path(__file__).resolve().parents[1]/'run-mt-codec-tests.py'
spec=importlib.util.spec_from_file_location('mt_codec_runner',runner)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
OID='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'

class StockAdmission(unittest.TestCase):
 def test_clean_exact_tree_admitted(self):
  with patch.object(module.subprocess,'check_output',side_effect=[OID+'\n','']) as call:
   self.assertEqual(module.verify_stock(Path('/inert/stock')),OID);self.assertEqual(call.call_count,2)
 def test_wrong_oid_refused_before_status(self):
  with patch.object(module.subprocess,'check_output',return_value='0'*40+'\n') as call:
   with self.assertRaisesRegex(RuntimeError,'Wrong stock oracle'):module.verify_stock(Path('/inert/stock'))
   self.assertEqual(call.call_count,1)
 def test_tracked_drift_refused(self):
  with patch.object(module.subprocess,'check_output',side_effect=[OID+'\n',' M src/mesh/generated/meshtastic/config.pb.cpp\n']):
   with self.assertRaisesRegex(RuntimeError,'drift'):module.verify_stock(Path('/inert/stock'))
 def test_untracked_drift_refused(self):
  with patch.object(module.subprocess,'check_output',side_effect=[OID+'\n','?? src/mesh/generated/meshtastic/injected.pb.cpp\n']):
   with self.assertRaisesRegex(RuntimeError,'drift'):module.verify_stock(Path('/inert/stock'))

if __name__=='__main__':unittest.main()
