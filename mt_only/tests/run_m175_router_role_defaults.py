#!/usr/bin/env python3
"""Inert-by-default M175 native ROUTER-default projection fixture."""
import argparse, hashlib, json, os, shlex, shutil, subprocess
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--run',action='store_true')
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if not args.run: parser.error('inert by default; pass --run to compile and execute')
repo=args.source.resolve();app=repo/'mt_only';proto=repo/'zephcore/bench/upstream-differential/source';out=args.output.resolve()
donor=proto/'NodeDB.cpp'
header=app/'src/modules/RouterRoleDefaults.h'
case=Path(__file__).resolve().with_name('m175_router_role_defaults_tests.cpp')
incs=[app/'src/modules/RouterRoleDefaults.initConfigIntervals.inc',
      app/'src/modules/RouterRoleDefaults.initModuleConfigIntervals.inc',
      app/'src/modules/RouterRoleDefaults.installRouterRoleDefaults.inc',
      app/'src/modules/RouterRoleDefaults.stockDefaults.inc']
required=[header,case,*incs,donor,app/'src/Config.h',app/'src/DeviceRolePolicy.h',
          app/'src/ModuleSettings.h',app/'src/gnss/GnssSettings.h',app/'src/Protobuf.cpp']
if any(not p.is_file() for p in required): parser.error('--source or pinned donor input is incomplete')
if hashlib.sha256(donor.read_bytes()).hexdigest()!='82c1491c1542fb96e28a653b1e8e381f44996ff67d5c2256d3564bf990f9534c':
 parser.error('stock NodeDB.cpp does not match the recorded M175 donor hash')
if out.exists() or out.is_relative_to(repo): parser.error('--output must be fresh and outside source tree')
out.mkdir(parents=True);gen=out/'generated';(gen/'meshtastic').mkdir(parents=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,gen/'meshtastic'/h.name)
cfiles=[proto/'nanopb'/x for x in ('pb_common.c','pb_encode.c','pb_decode.c')]
tracked=[*required,*proto.glob('*.pb.h'),*cfiles]
def digest():
 return {str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:'+x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in tracked}
before=digest()
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
vendor=repo/'zephcore/adapters/bench/vendor'
flags=['-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1','-Wall','-Wextra','-Werror',
       f'-I{app/"src"}',f'-I{vendor}',f'-I{gen}',f'-I{proto/"nanopb"}']
variants=[]
for name,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
 d=out/name;d.mkdir();objects=[];logs=[]
 cflags=['-std=c11','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1','-Wall','-Wextra','-Werror',f'-I{gen}',f'-I{proto/"nanopb"}']
 for index,src in enumerate(cfiles):
  obj=d/f'c-{index}.o';cmd=[*cc,*cflags,*extra,'-c',str(src),'-o',str(obj)];logs.append(shlex.join(cmd))
  result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
  logs.append(result.stdout)
  if result.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} C compile failed')
  objects.append(obj)
 for index,src in enumerate([app/'src/Protobuf.cpp',case]):
  obj=d/f'cpp-{index}.o';cmd=[*cxx,*flags,*extra,'-c',str(src),'-o',str(obj)];logs.append(shlex.join(cmd))
  result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180);logs.append(result.stdout)
  if result.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} C++ compile failed')
  objects.append(obj)
 binary=d/'m175-router-role-defaults';cmd=[*cxx,'-fsanitize=address,undefined','-g','-O1',*extra,*map(str,objects),'-o',str(binary)];logs.append(shlex.join(cmd))
 result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180);logs.append(result.stdout)
 if result.returncode:(d/'build.log').write_text('\n'.join(logs));raise SystemExit(f'{name} link failed')
 result=subprocess.run([str(binary)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 logs.append(shlex.join([str(binary)]));logs.append(result.stdout);(d/'build.log').write_text('\n'.join(logs))
 if result.returncode:raise SystemExit(f'{name} fixture failed')
 variants.append({'mode':name,'status':'PASS','binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                  'log_sha256':hashlib.sha256((d/'build.log').read_bytes()).hexdigest(),'stdout':result.stdout.strip()})
if before!=digest():raise SystemExit('SOURCE_CHANGED_DURING_TEST')
result={'schema':'m175-router-role-defaults-native/v1','status':'PASS','source_unchanged':True,
        'donor_commit':'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c',
        'donor_default_h_sha256':'a4211f1e26e7696a4378e1d8aa5203571d700e5b2de7be6e55cd5dab69c96fd7',
        'donor_hashes':before,
        'variants':variants,'target_build':False,'hardware':False,'network':False}
(out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print(json.dumps({'status':'PASS','variants':len(variants),'source_unchanged':True}))
