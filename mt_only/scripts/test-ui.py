#!/usr/bin/env python3
"""Compile actual renderer/model under ASAN/UBSAN and save synthetic compact/OLED/wide views."""
import argparse,json,os,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=Path(__file__).resolve().parents[1]
proto=app.parent/'zephcore/bench/upstream-differential/source'
generated=out/'protobuf'/'meshtastic';generated.mkdir(parents=True,exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,generated/header.name)
includes=['-I'+str(out/'protobuf'),'-I'+str(proto/'nanopb'),
          '-I'+str(app.parent/'zephcore/adapters/bench/vendor'),
          '-I'+str(app.parent/'zephcore/lib/monocypher')]
reports=[]
cxx=shlex.split(os.environ.get('CXX','g++'))
for name,width,height,mono,rows in [('compact',160,80,0,80),('compact-strips-16',160,80,0,16),('compact-strips-8',160,80,0,8),('mono',128,64,1,64),('wide',240,135,0,135),('wide-strips',240,135,0,16)]:
 target=out/name;target.mkdir(exist_ok=True)
 cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(app/'src'),*includes,f'-DCONFIG_MT_UI_WIDTH={width}',f'-DCONFIG_MT_UI_HEIGHT={height}',f'-DCONFIG_MT_UI_MONO={mono}',f'-DCONFIG_MT_UI_STRIP_ROWS={rows}',str(app/'tests/ui_tests.cpp'),'-o',str(target/'ui-tests')]
 subprocess.run(cmd,check=True)
 r=subprocess.run([str(target/'ui-tests'),str(target)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
 (target/'test.log').write_text(r.stdout+r.stderr);print(name,r.stdout+r.stderr);r.check_returncode()
 reports.append({'PASS':True,'ASAN_UBSAN':True,'hardware':False,'physical_colors_verified':False,'dimensions':[width,height],'mono':bool(mono),'synthetic_frames':len(list(target.glob('*.ppm'))),'command':cmd})
 for page in range(6):
  normal=(target/f'orientation-normal-{page}.ppm').read_bytes().split(b'\n',3)[3]
  flipped=(target/f'orientation-flipped-{page}.ppm').read_bytes().split(b'\n',3)[3]
  assert len(normal)==len(flipped)==width*height*3
  expected=b''.join(normal[i:i+3] for i in range(len(normal)-3,-1,-3))
  assert flipped==expected,(name,page,'180-degree orientation mismatch')
(out/'results.json').write_text(json.dumps(reports,indent=2)+'\n')
for file in (out/'wide').glob('*.ppm'):
 assert file.read_bytes()==(out/'wide-strips'/file.name).read_bytes(),file.name
print('PASS wide full-frame and 16-row strip renders match byte for byte')
print('PASS all six pages rotate exactly 180 degrees in every backend')

for file in (out/"compact").glob("*.ppm"):
 for strips in ("compact-strips-16","compact-strips-8"):
  assert file.read_bytes()==(out/strips/file.name).read_bytes(),(file.name,strips)
print("PASS compact full-frame, 16-row and 8-row strip renders match byte for byte")
