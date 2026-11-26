#!/usr/bin/env python3
"""Host-only, inert-by-default M139 coordinated Device/module commit fixture."""
import argparse,hashlib,json,os,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--run',action='store_true');p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not a.run:p.error('inert by default; pass --run to compile and execute')
repo=a.source.resolve();app=repo/'mt_only';proto=repo/'zephcore/bench/upstream-differential/source';vendor=repo/'zephcore/adapters/bench/vendor';mono=repo/'zephcore/lib/monocypher';out=a.output.resolve()
case=Path(__file__).resolve().with_name('m139_tak_device_commit_tests.cpp')
need=[app/'src/DeviceSettingsCommit.h',app/'src/SettingsCommit.h',app/'src/SettingsEdit.h',app/'src/ModuleEdit.h',app/'src/ModuleJournal.h',app/'src/ModuleSettings.h',app/'src/SettingsJournal.h',app/'src/Service.h',app/'src/modules/StoreForwardEdit.h',app/'src/modules/StoreForwardJournal.h',app/'src/modules/StoreForwardSettings.h',app/'src/modules/RouterRoleDefaults.h',app/'src/modules/RouterRoleDefaults.initConfigIntervals.inc',app/'src/modules/RouterRoleDefaults.initModuleConfigIntervals.inc',app/'src/modules/RouterRoleDefaults.installRouterRoleDefaults.inc',app/'src/modules/RouterRoleDefaults.stockDefaults.inc',app/'src/notifications/NotificationEdit.h',app/'src/notifications/NotificationJournal.h',app/'tests/known_channel_fixture.h',case]
if any(not x.is_file() for x in need):p.error('--source must be the M139 ZephTastic worktree')
if out.exists() or out.is_relative_to(repo):p.error('--output must be fresh and outside source tree')
out.mkdir(parents=True);gen=out/'generated';(gen/'meshtastic').mkdir(parents=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,gen/'meshtastic'/h.name)
cfiles=[proto/'nanopb'/x for x in ('pb_common.c','pb_encode.c','pb_decode.c')]+[mono/'monocypher.c',mono/'monocypher-ed25519.c']
cppfiles=[app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',case]
inputs=need+[app/'src/DeviceRolePolicy.h',app/'src/modules/RouterLateRoleDefaults.h',app/'src/NodeJournal.h',app/'src/notifications/NotificationSettings.h',app/'src/notifications/NotificationConfig.h',vendor/'MtNodeInfo.h',vendor/'MtCodec.h',vendor/'MtSignature.h',*proto.glob('*.pb.h'),*cfiles]
def digest(x):return hashlib.sha256(x.read_bytes()).hexdigest()
before={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+str(x):digest(x) for x in inputs}
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
inc=[f'-I{app/"src"}',f'-I{app/"tests"}',f'-I{vendor}',f'-I{mono}',f'-I{gen}',f'-I{proto/"nanopb"}']
variants=[]
for name,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
 d=out/name;d.mkdir();objects=[];log=[]
 cflags=['-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*inc]
 cppflags=['-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*inc]
 for i,src in enumerate(cfiles):
  obj=d/f'c-{i}.o';cmd=[*cc,*cflags,*extra,'-c',str(src),'-o',str(obj)];log.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});log.append(r.stdout)
  if r.returncode:(d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name} C compile failed: {src.name}')
  objects.append(obj)
 for i,src in enumerate(cppfiles):
  obj=d/f'cpp-{i}.o';cmd=[*cxx,*cppflags,*extra,'-c',str(src),'-o',str(obj)];log.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});log.append(r.stdout)
  if r.returncode:(d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name} C++ compile failed: {src.name}')
  objects.append(obj)
 binary=d/'m139-tak-device-commit';cmd=[*cxx,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*extra,*map(str,objects),'-o',str(binary)];log.append(shlex.join(cmd));r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);log.append(r.stdout)
 if r.returncode:(d/'build.log').write_text('\n'.join(log));raise SystemExit(f'{name} link failed')
 r=subprocess.run([str(binary)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});log.append(shlex.join([str(binary)]));log.append(r.stdout);(d/'build.log').write_text('\n'.join(log))
 if r.returncode:raise SystemExit(f'{name} fixture failed')
 variants.append({'mode':name,'status':'PASS','binary_sha256':digest(binary),'log_sha256':digest(d/'build.log'),'stdout':r.stdout.strip()})
after={str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+str(x):digest(x) for x in inputs}
if before!=after:raise SystemExit('SOURCE_CHANGED_DURING_TEST')
result={'schema':'m139-tak-device-commit-native/v1','status':'PASS','source_unchanged':True,'inputs_sha256':before,'variants':variants,'target_build':False,'hardware':False,'network':False}
(out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');print(json.dumps({'status':'PASS','variants':len(variants),'source_unchanged':True}))
