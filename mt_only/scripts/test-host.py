#!/usr/bin/env python3
import argparse,json,os,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=Path(__file__).resolve().parents[1];root=app.parent;sys.path.insert(0,str(root));import host_test_toolchain as host
vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),*host.crypto_compile_flags()]
commands=[];objects=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*host.cc(),'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/service_tests.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host.crypto_link_flags(),'-o',str(out/'service-tests')]
subprocess.run(cmd,check=True);commands.append(cmd)
r=subprocess.run([str(out/'service-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'service.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'hardware':False})+'\n')
