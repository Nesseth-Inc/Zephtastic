#!/usr/bin/env python3
"""Exercise exact Companion-to-production-Controller-to-Display path; no hardware."""
import argparse,hashlib,json,os,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
display=(app/'src/ui/Display.cpp').read_text();body=display[display.index('namespace mt::ui {'):]
# device_config_fixture has a global helper named device(); qualify the Zephyr
# struct tag in this generated host translation unit without changing source.
host_body=body.replace('static const device *const','static const struct device *const')
fixture=(app/'tests/input_display_bridge_fixture.cpp').read_text();marker='// INCLUDE_ACTUAL_DISPLAY_BODY'
if fixture.count(marker)!=1:raise RuntimeError('bridge fixture marker must occur exactly once')
generated=out/'input-display-bridge.cpp';generated.write_text(fixture.replace(marker,host_body))
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1','-DCONFIG_MT_DISPLAY=1','-DCONFIG_MT_T096_OWNER_TX=1','-DCONFIG_MT_UI_DEVICE_LABEL="HELTEC T096"','-DCONFIG_MT_UI_WIDTH=160','-DCONFIG_MT_UI_HEIGHT=80','-DCONFIG_MT_UI_MONO=0','-DCONFIG_MT_UI_STRIP_ROWS=16']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
exe=out/'input-display-bridge';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(generated),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(exe)]
commands.append(cmd);subprocess.run(cmd,check=True)
result=subprocess.run([str(exe)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr);result.check_returncode()
sources=[app/'src/Companion.h',app/'src/main.cpp',app/'src/ui/Controller.h',app/'src/ui/Display.h',app/'src/ui/Display.cpp',app/'src/ui/Model.h',app/'tests/input_display_bridge_fixture.cpp',Path(__file__).resolve()]
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'short_enums':a.short_enums,'hardware':False,'transport_cases':['framed USB','packet BLE'],'production_bridge':True,'production_display_body_sha256':hashlib.sha256(body.encode()).hexdigest(),'source_sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},'scenario_groups':1},indent=2)+'\n')
