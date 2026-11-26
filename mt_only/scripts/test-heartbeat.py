#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual heartbeat policy/journal/Device commit ASAN+UBSAN tests; RAM only."""
if not __debug__: raise RuntimeError('optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve()
if out.exists() or out.is_relative_to(root):p.error('fresh output outside source required')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
out.mkdir(parents=True);(out/'meshtastic').mkdir()
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
sha=lambda b:hashlib.sha256(b).hexdigest()
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
flags=['-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];link=['-lcrypto']
suites=[('heartbeat_pulse',4),('heartbeat_controller',4),('notification_journal_migration',6),('device_heartbeat_commit',7)]
common=[mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c']],app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp']
rows=[];script_sha=sha(Path(__file__).read_bytes())
result={'status':'FAIL','hardware':False,'network':False,'memory_backend_only':True,'synthetic_radio_only':True,'ASAN_UBSAN':True,'variants':rows,'source_stable':False,'script_sha256':script_sha}
def pin(path):
 path=path.resolve()
 if path.parent==out/'meshtastic':
  original=proto/path.name;assert path.read_bytes()==original.read_bytes();path=original
 return str(path),sha(path.read_bytes())
def dependencies(path):return [Path(n) for n in shlex.split(path.read_text().replace('\\\n',' ').split(':',1)[1])]
try:
 version=subprocess.run([*cxx,'--version'],capture_output=True,text=True,check=True,timeout=15).stdout
 (out/'compiler.txt').write_text(version);result['compiler_version_sha256']=sha(version.encode())
 for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
  for suite,groups in suites:
   folder=out/(label+'-'+suite);folder.mkdir();objects=[];commands=[];inputs={}
   sources=([*common] if suite=='device_heartbeat_commit' else [])+[app/'tests'/(suite+'_tests.cpp')]
   with(folder/'build.log').open('x')as log:
    for index,source in enumerate(sources):
     compiler=cc if source.suffix=='.c' else cxx;language=['-std=c11']if source.suffix=='.c'else['-std=c++17','-Wall','-Wextra','-Werror']
     dep=folder/(str(index)+'.d');obj=folder/(str(index)+'.o')
     command=[*compiler,*language,*flags,*extra,'-MM','-MT',str(index),'-MF',str(dep),str(source)];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
     selected=dependencies(dep);before=dict(pin(path)for path in selected);inputs.update(before)
     command=[*compiler,*language,*flags,*extra,'-c',str(source),'-o',str(obj)];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
     assert dict(pin(path)for path in selected)==before;objects.append(str(obj))
    command=[*cxx,*flags,*extra,*objects,*link,'-o',str(folder/'tests')];commands.append(command);subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
   execute=[str(folder/'tests')]
   with(folder/'test.log').open('x')as log:subprocess.run(execute,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
   lines=(folder/'test.log').read_text().splitlines();assert sum(s.startswith('PASS ')for s in lines)==groups and lines[-1]==suite.upper()+'_GROUPS '+str(groups)
   assert all(sha(Path(n).read_bytes())==digest for n,digest in inputs.items())
   rows.append({'variant':label,'suite':suite,'groups':groups,'commands':commands,'execution_command':execute,'compiled_input_sha256':inputs,'binary_sha256':sha((folder/'tests').read_bytes()),'test_log_sha256':sha((folder/'test.log').read_bytes())})
 assert len(rows)==8 and sha(Path(__file__).read_bytes())==script_sha
 for row in rows:assert all(sha(Path(n).read_bytes())==digest for n,digest in row['compiled_input_sha256'].items())
 result.update(status='PASS',source_stable=True,groups_per_enum=dict(suites))
finally:(out/'results.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print(json.dumps({'status':result['status'],'executions':len(rows),'groups_per_enum':dict(suites),'ASAN_UBSAN':True,'hardware':False,'network':False}))
