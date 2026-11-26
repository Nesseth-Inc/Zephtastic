#!/usr/bin/env python3
"""Compile independent M8 review probes against an exact selected source tree."""
import argparse,hashlib,json,os,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--test',default='m8_independent_routing.cpp')
a=p.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
local=Path(__file__).resolve().parents[1];test=local/'tests'/a.test
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g',*[f'-I{x}' for x in (root/'mt_only/src',vendor,mono,out,proto/'nanopb')]]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
host_link=[]
if os.sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
commands=[];objects=[]
def state():
  paths=[v for folder in (root/'mt_only/src',vendor,mono,proto) for v in folder.rglob('*') if v.is_file() and v.suffix in ('.h','.c','.cpp')]
  return {str(v.relative_to(root)):hashlib.sha256(v.read_bytes()).hexdigest() for v in sorted(paths)}
before=state()
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c')]]:
  obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)]
  subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(test),str(root/'mt_only/src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host_link,'-o',str(out/'review-tests')]
subprocess.run(cmd,check=True);commands.append(cmd)
r=subprocess.run([str(out/'review-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
after=state();stable=before==after
try:head=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True,stderr=subprocess.DEVNULL).strip() if (root/'.git').exists() else None
except (OSError,subprocess.CalledProcessError):head=None # Exported source remains testable; exact file hashes are retained.
result={'source_commit':head,'source_stable':stable,'hardware':False,'ASAN_UBSAN':True,'exit_code':r.returncode,'status':'PASS' if stable and r.returncode==0 else 'FAIL','tests_sha256':{str(v.relative_to(local)):hashlib.sha256(v.read_bytes()).hexdigest() for v in (test,local/'tests/conformance_fixture.h',Path(__file__).resolve())},'cases':[{'status':line.split(' ',1)[0],'name':line.split(' ',1)[1]} for line in r.stdout.splitlines() if line.startswith(('PASS ','FAIL '))]}
(out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n');(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n')
print(r.stdout+r.stderr,end='');raise SystemExit(r.returncode if stable else 2)
