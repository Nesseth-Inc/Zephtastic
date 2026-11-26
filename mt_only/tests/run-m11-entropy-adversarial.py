#!/usr/bin/env python3
"""Independent bounded-entropy interface checks; synthetic ports, no devices."""
import argparse,hashlib,json,os,subprocess,shutil,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);(out/'RESULTS.json').unlink(missing_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host;import source_provenance as provenance
runner=Path(__file__).resolve();test=runner.parent/'m11_entropy_adversarial.cpp';mono=root/'zephcore/lib/monocypher'
def sha(x):return hashlib.sha256(x.read_bytes()).hexdigest()
def state():
 return {str(x.relative_to(root)):sha(x) for d in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,root/'zephcore/adapters/bench/vendor',root/'zephcore/bench/upstream-differential/source') for x in sorted(d.rglob('*')) if x.is_file() and x.suffix in ('.c','.cpp','.h')}
stock=root/'zephcore/bench/upstream-differential/source'
mesh=out/'includes/meshtastic';mesh.mkdir(parents=True,exist_ok=True)
for h in stock.glob('*.pb.h'):shutil.copyfile(h,mesh/h.name)
before=state();source_info=provenance.source_provenance(root);test_hash=sha(test);runner_hash=sha(runner)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'mt_only/src'),'-I'+str(mono),'-I'+str(root/'zephcore/adapters/bench/vendor'),'-I'+str(root/'mt_only/experimental/pki'),'-I'+str(out/'includes'),'-I'+str(stock/'nanopb')]
commands=[[*host.cc(),'-std=c11',*flags,'-c',str(mono/'monocypher.c'),'-o',str(out/'monocypher.o')],[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(test),str(root/'mt_only/src/pki/BoundedEntropy.cpp'),str(out/'monocypher.o'),'-o',str(out/'entropy-tests')]]
for cmd in commands:subprocess.run(cmd,check=True)
r=subprocess.run([str(out/'entropy-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=state()==before and source_info==provenance.source_provenance(root) and sha(test)==test_hash and sha(runner)==runner_hash
(out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n');(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
result={**source_info,'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_stable':stable,'ASAN_UBSAN':True,'hardware':False,'entropy_quality_tested':False,'real_target_port_tested':False,'groups':r.stdout.splitlines(),'exit_code':r.returncode,'test_sha256':test_hash,'runner_sha256':runner_hash}
(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n');raise SystemExit(r.returncode if stable else 2)
