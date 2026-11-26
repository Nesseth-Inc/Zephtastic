#!/usr/bin/env python3
"""M22 XIAO/T096 logical USB stream boundaries; memory-only ASAN/UBSAN."""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host
vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),*host.crypto_compile_flags()]
commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*host.cc(),'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
test=app/'tests/usb_stream_boundary_tests.cpp';binary=out/'usb-stream-boundary-tests'
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(test),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host.crypto_link_flags(),'-o',str(binary)]
commands.append(cmd);subprocess.run(cmd,check=True)
run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=120,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
sources=[root/'host_test_toolchain.py',app/'src/Companion.h',app/'src/UsbSessionEpoch.h',app/'src/UsbSerialJtagTx.h',app/'tests/node_mute_fixture.h',test,Path(__file__).resolve()]
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'hardware':False,'real_usb':False,'groups':sum(line.startswith('PASS ') for line in run.stdout.splitlines())-1,'source_sha256':{str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}},indent=2)+'\n')
