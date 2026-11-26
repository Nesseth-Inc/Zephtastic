#!/usr/bin/env python3
"""Host-only, inert-by-default TAK_TRACKER role-default projection fixture."""
import argparse, hashlib, json, os, shlex, shutil, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--run',action='store_true');p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not a.run:p.error('inert by default; pass --run to compile and execute')
repo=a.source.resolve();app=repo/'mt_only';proto=repo/'zephcore/bench/upstream-differential/source';out=a.output.resolve()
header=app/'src/modules/TakRoleDefaults.h';case=Path(__file__).resolve().with_name('m139_tak_defaults_tests.cpp')
if not header.is_file() or not (app/'src/Config.h').is_file() or not case.is_file():p.error('--source must be the M139 ZephTastic worktree')
if out.exists() or out.is_relative_to(repo):p.error('--output must be fresh and outside source tree')
out.mkdir(parents=True);gen=out/'generated';(gen/'meshtastic').mkdir(parents=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,gen/'meshtastic'/h.name)
cfiles=[proto/'nanopb'/x for x in ('pb_common.c','pb_encode.c','pb_decode.c')]
paths=[header,case,Path(__file__).resolve(),app/'src/Config.h',app/'src/DeviceRolePolicy.h',app/'src/ModuleSettings.h',app/'src/gnss/GnssSettings.h',app/'src/Protobuf.cpp',*proto.glob('*.pb.h'),*cfiles]
inputs={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));vendor=repo/'zephcore/adapters/bench/vendor';flags=['-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1','-Wall','-Wextra','-Werror','-DCONFIG_MT_TAK_TRACKER=1',f'-I{app/"src"}',f'-I{vendor}',f'-I{gen}',f'-I{proto/"nanopb"}']
variants=[]
for name,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
 d=out/name;d.mkdir();objects=[];logs=[]
 cflags=['-std=c11','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1','-Wall','-Wextra','-Werror',f'-I{gen}',f'-I{proto/"nanopb"}']
 for index,src in enumerate(cfiles):
  obj=d/f'c-{index}.o';cmd=[*cc,*cflags,*extra,'-c',str(src),'-o',str(obj)];logs.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});logs.append(r.stdout)
  if r.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} C compile failed')
  objects.append(obj)
 for index,src in enumerate([app/'src/Protobuf.cpp',case]):
  obj=d/f'cpp-{index}.o';cmd=[*cxx,*flags,*extra,'-c',str(src),'-o',str(obj)];logs.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});logs.append(r.stdout)
  if r.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} C++ compile failed')
  objects.append(obj)
 binary=d/'m139-tak-defaults';cmd=[*cxx,'-fsanitize=address,undefined','-g','-O1',*extra,*map(str,objects),'-o',str(binary)];logs.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);logs.append(r.stdout)
 if r.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} link failed')
 r=subprocess.run([str(binary)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});logs.append(shlex.join([str(binary)]));logs.append(r.stdout);(d/'build.log').write_text('\n'.join(logs))
 if r.returncode:raise SystemExit(f'{name} fixture failed')
 variants.append({'mode':name,'status':'PASS','binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'log_sha256':hashlib.sha256((d/'build.log').read_bytes()).hexdigest(),'stdout':r.stdout.strip()})
after={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}
if inputs!=after:raise SystemExit('SOURCE_CHANGED_DURING_TEST')
result={'schema':'m139-tak-role-defaults-native/v1','status':'PASS','source_unchanged':True,'inputs_sha256':inputs,'variants':variants,'target_build':False,'hardware':False,'network':False}
(out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');print(json.dumps({'status':'PASS','variants':len(variants),'source_unchanged':True}))
