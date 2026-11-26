#!/usr/bin/env python3
"""M26 absent favorite native tests; memory-only, no hardware or RF."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
exe=out/'favorite-noop-tests';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/favorite_noop_tests.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects]
if sys.platform.startswith('linux'):cmd+=['-lcrypto']
cmd+=['-o',str(exe)];commands.append(cmd);subprocess.run(cmd,check=True)
run=subprocess.run([str(exe)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr);run.check_returncode()
sources=[app/'src/Companion.h',app/'src/NodeCache.h',app/'src/NodeJournal.h',app/'src/NodeAdmin.h',app/'src/UsbSessionEpoch.h',app/'tests/node_mute_fixture.h',app/'tests/favorite_noop_tests.cpp',Path(__file__).resolve()]
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'optimized':True,'short_enums':a.short_enums,'hardware':False,'RF':False,'scenario_groups':sum(line.startswith('PASS ') for line in run.stdout.splitlines())-1,'board_transport_matrix_cases':4,'node_journal_record_size':1492,'source_sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}},indent=2)+'\n')
