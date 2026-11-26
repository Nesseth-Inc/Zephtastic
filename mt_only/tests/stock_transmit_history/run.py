#!/usr/bin/env python3
"""Build exact donor-clock, committed storage, factory and scheduler tests in both enum ABIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

if not __debug__:raise RuntimeError('optimized verification disabled')
here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--stock',type=Path,help='verify exact pinned files against a local donor checkout')
args=p.parse_args()
out=args.output.resolve()
if out.exists() or out.is_relative_to(root):p.error('provide a fresh evidence directory outside source')
out.mkdir(parents=True)
lock=json.loads((here/'source-lock.json').read_text())
if lock['commit']!='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c':raise ValueError('wrong donor')
for row in lock['slices']:
    raw=(here/row['file']).read_bytes()
    assert hashlib.sha256(raw).hexdigest()==row['sha256']
    if args.stock:
        source=subprocess.check_output(['git','-C',args.stock,'show',lock['commit']+':'+row['path']])
        first=row['start']-1
        assert b''.join(source.splitlines(keepends=True)[first:first+len(raw.splitlines())])==raw
proto=root/'zephcore/bench/upstream-differential/source'
vendor=root/'zephcore/adapters/bench/vendor'
mono=root/'zephcore/lib/monocypher'
result=[]
for label,abi in [('normal',[]),('short',['-fshort-enums'])]:
    build=out/label;build.mkdir()
    (build/'meshtastic').mkdir()
    for header in proto.glob('*.pb.h'):(build/'meshtastic'/header.name).write_bytes(header.read_bytes())
    # Test only the clock/sensor injection boundary. The submitted ownerPoll
    # body and its encoder compile unchanged in the harness.
    unit=(app/'src/modules/CompletionRuntime.h').read_text()
    parts=[]
    for marker in ('  static bool encode(', '  template<class Sink>void ownerPoll('):
        start=unit.index(marker);end=unit.index('\n  }',start)+4;parts.append(unit[start:end])
    (build/'sensor-owner.inc').write_text('\n'.join(parts)+'\n')
    fixture=(app/'tests/beacon_controller_tests.cpp').read_text()
    (build/'beacon-harness.inc').write_text(fixture[:fixture.index('int main(){')])
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DCONFIG_MT_PERSIST_BROADCAST_HISTORY=1',*abi,
           *['-I'+str(path) for path in (build,app/'src',app/'tests',app/'tests/beacon/stubs',vendor,mono,proto/'nanopb')],*host.crypto_compile_flags()]
    if sys.platform=='darwin':flags.append('-DEKEYREJECTED=129')
    def run(command):
        with (build/'commands.log').open('a') as log:
            log.write(shlex.join(str(x) for x in command)+'\n')
        return subprocess.run(command,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
    objects=[]
    for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c')]]:
        obj=build/(src.stem+'.o');run([*host.cc(),'-std=c11',*flags,'-c',src,'-o',obj]);objects.append(obj)
    for test,extras in [(here/'history.cpp',[]),(here/'producers.cpp',[]),
                        (here/'beacon.cpp',[app/'src/modules/BeaconTargetRuntime.cpp',app/'src/modules/BeaconExcursion.cpp']),
                        (app/'tests/stock_admin_routing/factory.cpp',[])]:
        name=test.stem
        run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,test,*extras,app/'src/Protobuf.cpp',
             vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objects,*host.crypto_link_flags(),'-o',build/name])
        run([build/name]);result.append({'enum':label,'case':name})
(out/'result.json').write_text(json.dumps({'stock':lock['commit'],'tests':result,'asan_ubsan':True,'hardware':False},indent=2)+'\n')
