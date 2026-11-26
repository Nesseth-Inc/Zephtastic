#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual Companion/editor/journal/controller memory tests; no devices/network."""
if not __debug__: raise RuntimeError('optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--suite',choices=['notification','position'],default='notification');a=p.parse_args();out=a.output.resolve()
if out.exists() or out.is_relative_to(root):p.error('fresh output outside source required')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
out.mkdir(parents=True);(out/'meshtastic').mkdir()
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
sha=lambda b:hashlib.sha256(b).hexdigest()
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
flags=['-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];link=['-lcrypto']
rows=[];result={'status':'FAIL','suite':a.suite,'hardware':False,'network':False,'ASAN_UBSAN':True,'variants':rows,'source_stable':False}
groups=9 if a.suite=='notification'else 4
sources=[mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c']],app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',app/'tests'/(a.suite+'_runtime_tests.cpp')]
def pin(path):
 path=path.resolve()
 if path.parent==out/'meshtastic':
  original=proto/path.name;assert path.read_bytes()==original.read_bytes();path=original
 return str(path),sha(path.read_bytes())
try:
 for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
  folder=out/label;folder.mkdir();objects=[];commands=[];inputs={}
  with(folder/'build.log').open('x')as log:
   for index,source in enumerate(sources):
    compiler=cc if source.suffix=='.c' else cxx;language=['-std=c11']if source.suffix=='.c'else['-std=c++17','-Wall','-Wextra','-Werror']
    dep=folder/(str(index)+'.d');obj=folder/(str(index)+'.o')
    command=[*compiler,*language,*flags,*extra,'-MM','-MT',str(index),'-MF',str(dep),str(source)];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
    selected=[Path(n)for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])]
    before=dict(pin(path)for path in selected);inputs.update(before)
    command=[*compiler,*language,*flags,*extra,'-c',str(source),'-o',str(obj)];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    assert dict(pin(path)for path in selected)==before;objects.append(str(obj))
   command=[*cxx,*flags,*extra,*objects,*link,'-o',str(folder/'tests')];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
  with(folder/'test.log').open('x')as log:subprocess.run([str(folder/'tests')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
  lines=(folder/'test.log').read_text().splitlines();assert sum(s.startswith('PASS ')for s in lines)==groups and lines[-1]==a.suite.upper()+'_RUNTIME_GROUPS '+str(groups)
  assert all(sha(Path(n).read_bytes())==digest for n,digest in inputs.items())
  rows.append({'variant':label,'groups':groups,'commands':commands,'compiled_inputs':inputs,'binary_sha256':sha((folder/'tests').read_bytes()),'test_log_sha256':sha((folder/'test.log').read_bytes())})
 assert len(rows)==2;result.update(status='PASS',source_stable=True)
finally:(out/'results.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print(json.dumps({'status':result['status'],'suite':a.suite,'executions':len(rows),'groups_per_enum':groups,'ASAN_UBSAN':True,'hardware':False}))
