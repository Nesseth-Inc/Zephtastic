#!/usr/bin/env python3
"""Independent memory-only ledger probes against a selected frozen source tree."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host;import source_provenance as provenance
(out/'RESULTS.json').unlink(missing_ok=True)
test=Path(__file__).resolve().parent/'m10_ledger_adversarial.cpp';mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
def state():
    paths=[x for d in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor) for x in d.rglob('*') if x.is_file() and x.suffix in ('.cpp','.c','.h')]
    return {str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(paths)}
before=state();source_info=provenance.source_provenance(root)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',*['-I'+str(x) for x in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor)]]
commands=[];objects=[]
def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
for name in ('monocypher.c','monocypher-ed25519.c'):
    obj=out/(name+'.o');run([*host.cc(),'-std=c11',*flags,'-c',str(mono/name),'-o',str(obj)]);objects.append(str(obj))
sources=[test,root/'mt_only/src/pki/PkiIdentity.cpp',root/'mt_only/src/pki/PkiLedger.cpp',vendor/'MtSignature.cpp']
run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,sources),*objects,'-o',str(out/'ledger-tests')])
r=subprocess.run([str(out/'ledger-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=state()==before and source_info==provenance.source_provenance(root)
(out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n');(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
result={**source_info,'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_stable':stable,'ASAN_UBSAN':True,'hardware':False,'persistent_credentials':False,'storage':'in-memory fault-injected map only','groups':r.stdout.splitlines(),'exit_code':r.returncode,'test_sha256':hashlib.sha256(test.read_bytes()).hexdigest(),'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n');raise SystemExit(r.returncode if stable else 2)
