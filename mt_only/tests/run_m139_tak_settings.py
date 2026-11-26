#!/usr/bin/env python3
"""Focused host-only M139 TAK settings/journal test (inert without --run)."""
import argparse, hashlib, json, os, shlex, shutil, subprocess
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--run',action='store_true')
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not a.run:p.error('inert by default; pass --run to compile and execute')
repo=a.source.resolve(); app=repo/'mt_only'; proto=repo/'zephcore/bench/upstream-differential/source'
out=a.output.resolve()
if not (app/'src/modules/ExtendedModuleSettings.h').is_file() or not (app/'src/modules/ExtendedModuleJournal.h').is_file():
    p.error('--source must be the M139 ZephTastic worktree')
if out.exists() or out.is_relative_to(repo):p.error('--output must be fresh and outside the source tree')
out.mkdir(parents=True); gen=out/'generated'; (gen/'meshtastic').mkdir(parents=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,gen/'meshtastic'/header.name)
case=Path(__file__).resolve().with_name('m139_tak_settings_tests.cpp')
paths=[case,Path(__file__).resolve(),app/'src/modules/ExtendedModuleSettings.h',app/'src/modules/ExtendedModuleJournal.h',
       app/'src/modules/TrafficManagementSettings.h',app/'src/SettingsJournal.h',app/'src/Config.h',app/'src/Protobuf.cpp',
       *proto.glob('*.pb.h'),*(proto/'nanopb'/n for n in ('pb.h','pb_common.h','pb_encode.h','pb_decode.h','pb_common.c','pb_encode.c','pb_decode.c'))]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
before={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+x.name:digest(x) for x in paths}
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
vendor=repo/'zephcore/adapters/bench/vendor'; mono=repo/'zephcore/lib/monocypher'
flags=['-fsanitize=address,undefined','-g','-O1','-Wall','-Wextra','-Werror','-fno-omit-frame-pointer',
       f'-I{app/"src"}',f'-I{app/"tests"}',f'-I{vendor}',f'-I{mono}',f'-I{gen}',f'-I{proto/"nanopb"}']
cfiles=[proto/'nanopb'/x for x in ('pb_common.c','pb_encode.c','pb_decode.c')]
cppfiles=[app/'src/Protobuf.cpp',case]
variants=[];commands=[]
for name,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
    d=out/name;d.mkdir();objects=[];log=[]
    for i,src in enumerate(cfiles):
        obj=d/f'c-{i}.o';cmd=[*cc,'-std=c11',*flags,*extra,'-c',str(src),'-o',str(obj)]
        log.append(shlex.join(cmd));run=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
        log.append(run.stdout)
        if run.returncode: (d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name}: C compile failed: {src.name}')
        objects.append(obj)
    for i,src in enumerate(cppfiles):
        obj=d/f'cpp-{i}.o';cmd=[*cxx,'-std=c++17',*flags,*extra,'-c',str(src),'-o',str(obj)]
        log.append(shlex.join(cmd));run=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
        log.append(run.stdout)
        if run.returncode: (d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name}: C++ compile failed: {src.name}')
        objects.append(obj)
    binary=d/'m139-tak-settings';cmd=[*cxx,'-fsanitize=address,undefined','-g','-O1',*extra,*map(str,objects),'-o',str(binary)]
    log.append(shlex.join(cmd));run=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
    log.append(run.stdout)
    if run.returncode:(d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name}: link failed')
    run=subprocess.run([str(binary)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
    log.append(shlex.join([str(binary)]));log.append(run.stdout);(d/'build.log').write_text('\n'.join(log))
    if run.returncode:raise SystemExit(f'{name}: fixture failed')
    variants.append({'variant':name,'status':'PASS','binary_sha256':digest(binary),'build_log_sha256':digest(d/'build.log'),'stdout':run.stdout.strip()})
after={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+x.name:digest(x) for x in paths}
if before!=after:raise SystemExit('SOURCE_CHANGED_DURING_TEST')
result={'schema':'zt-m139-tak-settings-native/v1','status':'PASS','source_unchanged':True,'input_sha256':before,
        'sanitizers':['address','undefined'],'variants':variants,'target_build':False,'hardware':False,'network':False}
(out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print(json.dumps({'status':'PASS','variants':len(variants),'source_unchanged':True}))
