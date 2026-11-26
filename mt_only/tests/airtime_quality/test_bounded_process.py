"""Real OS subprocess controls, synthetic CPU/temporary-file work only."""
import json,os,pathlib,signal,subprocess,sys,tempfile,time,unittest
from unittest.mock import patch
from bounded_process import run_group
class BoundedTests(unittest.TestCase):
 def test_real_sigterm_cleans_owned_child_before_driver_exit(self):
  with tempfile.TemporaryDirectory()as d:
   root=pathlib.Path(d);pidfile=root/'owned.pid';receipt=root/'cleanup.json'
   child='import pathlib,os,sys,time;p=pathlib.Path(sys.argv[1]);tmp=p.with_suffix(".pending");tmp.write_text(str(os.getpid()));tmp.replace(p);time.sleep(30)'
   script='''import json,pathlib,sys
from bounded_process import run_group,install_termination_handler
install_termination_handler()
try:
 with open(sys.argv[1]+".log","w")as log:run_group([sys.executable,"-c",sys.argv[3],sys.argv[1]],timeout=20,stdout=log)
except BaseException as error:
 pathlib.Path(sys.argv[2]).write_text(json.dumps({"error_type":type(error).__name__,"cleanup":getattr(error,"owned_process_cleanup",{})}))
 raise SystemExit(9)
raise SystemExit(0)
'''
   process=subprocess.Popen([sys.executable,'-B','-c',script,str(pidfile),str(receipt),child],cwd=pathlib.Path(__file__).resolve().parent,start_new_session=True)
   try:
    deadline=time.monotonic()+3
    while not pidfile.exists():
     if process.poll()is not None or time.monotonic()>=deadline:self.fail('synthetic driver did not launch child')
     time.sleep(.01)
    pid=int(pidfile.read_text());os.kill(process.pid,signal.SIGTERM)
    self.assertEqual(process.wait(timeout=3),9)
    result=json.loads(receipt.read_text());self.assertEqual(result['error_type'],'TerminationRequested')
    self.assertEqual(result['cleanup'],{'leader_reaped':True,'group_gone':True})
    with self.assertRaises(ProcessLookupError):os.kill(pid,0)
   finally:
    if process.poll()is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=2)
    if pidfile.exists():
     try:os.killpg(int(pidfile.read_text()),signal.SIGKILL)
     except ProcessLookupError:pass
 def test_normal_exit_output_and_closed_group(self):
  with tempfile.TemporaryFile(mode='w+')as f:
   result=run_group([sys.executable,'-c','print("SYNTHETIC")'],timeout=2,stdout=f)
   f.seek(0);self.assertEqual(f.read(),'SYNTHETIC\n');self.assertEqual(result.returncode,0)
 def test_nonzero_exit_is_not_a_success(self):
  with tempfile.TemporaryFile(mode='w+')as f:
   with self.assertRaises(subprocess.CalledProcessError)as caught:run_group([sys.executable,'-c','raise SystemExit(7)'],timeout=2,stdout=f)
   self.assertEqual(caught.exception.returncode,7);self.assertTrue(caught.exception.owned_process_cleanup['leader_reaped'])
 def test_timeout_kills_actual_descendant_before_another_stage(self):
  with tempfile.TemporaryDirectory()as d,tempfile.TemporaryFile(mode='w+')as f:
   dest=pathlib.Path(d)/'child.pid'
   code='import pathlib,subprocess,sys,time;p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"]);pathlib.Path(sys.argv[1]).write_text(str(p.pid));time.sleep(30)'
   with self.assertRaises(subprocess.TimeoutExpired)as caught:run_group([sys.executable,'-c',code,str(dest)],timeout=.5,stdout=f)
   self.assertTrue(caught.exception.owned_process_cleanup['leader_reaped']);pid=int(dest.read_text());deadline=time.monotonic()+1
   while time.monotonic()<deadline:
    try:os.kill(pid,0)
    except ProcessLookupError:break
    stat=pathlib.Path('/proc')/str(pid)/'stat'
    if stat.exists()and stat.read_text().split()[2]=='Z':break
    time.sleep(.02)
   else:self.fail('owned descendant remains running')
 def test_interruption_cleans_owned_child_and_propagates(self):
  original=subprocess.Popen.wait;first=True
  def wait(p,*a,**kw):
   nonlocal first
   if first:first=False;raise KeyboardInterrupt()
   return original(p,*a,**kw)
  with tempfile.TemporaryFile(mode='w+')as f,patch.object(subprocess.Popen,'wait',wait):
   with self.assertRaises(KeyboardInterrupt)as caught:run_group([sys.executable,'-c','import time;time.sleep(30)'],timeout=2,stdout=f)
   self.assertTrue(caught.exception.owned_process_cleanup['leader_reaped'])
if __name__=='__main__':unittest.main(verbosity=2)
