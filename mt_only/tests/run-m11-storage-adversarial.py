#!/usr/bin/env python3
"""Independent memory-only storage lifecycle probes against a selected frozen source tree."""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'RESULTS.json').unlink(missing_ok=True)
test=Path(__file__).resolve().parent/'m11_storage_adversarial.cpp';mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
def state():
    paths=[x for d in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor) for x in d.rglob('*') if x.is_file() and x.suffix in ('.cpp','.c','.h')]
    return {str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(paths)}
def revision():
    if (root/'.git').exists():
        return subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    manifest=root.parent/'SOURCE-MANIFEST.json'
    return json.loads(manifest.read_text())['source_revision'] if manifest.exists() else None
before=state();head=revision();test_hash=hashlib.sha256(test.read_bytes()).hexdigest();runner_hash=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',*['-I'+str(x) for x in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor)]]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
commands=[];objects=[]
def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
for name in ('monocypher.c','monocypher-ed25519.c'):
    obj=out/(name+'.o');run([*cc,'-std=c11',*flags,'-c',str(mono/name),'-o',str(obj)]);objects.append(str(obj))
sources=[test,root/'mt_only/src/pki/PkiIdentity.cpp',root/'mt_only/src/pki/PkiLedger.cpp',root/'mt_only/src/pki/PkiStorage.cpp',root/'mt_only/src/pki/PkiAppendHistory.cpp',vendor/'MtSignature.cpp']
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,sources),*objects,'-o',str(out/'storage-tests')])
r=subprocess.run([str(out/'storage-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=state()==before and head==revision() and hashlib.sha256(test.read_bytes()).hexdigest()==test_hash and hashlib.sha256(Path(__file__).read_bytes()).hexdigest()==runner_hash
(out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n');(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
result={'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_commit':head,'source_stable':stable,'ASAN_UBSAN':True,'hardware':False,'persistent_credentials':False,'storage':'in-memory fault-injected map only','groups':r.stdout.splitlines(),'exit_code':r.returncode,'test_sha256':test_hash,'runner_sha256':runner_hash}
(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n');raise SystemExit(r.returncode if stable else 2)
