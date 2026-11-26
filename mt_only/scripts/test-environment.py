#!/usr/bin/env python3
"""Actual pure environment/clock adapters with nanopb; no device or RF access."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();out.mkdir(exist_ok=False)
proto=root/'zephcore/bench/upstream-differential/source';(out/'meshtastic').mkdir()
for f in proto.glob('*.pb.h'):shutil.copyfile(f,out/'meshtastic'/f.name)
inc=['-I'+str(v) for v in (app/'src',app/'src/environment',root/'zephcore/adapters/sensors',root/'zephcore/adapters/bench/vendor',out,proto/'nanopb')]
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer'];cc=shlex.split(os.getenv('CC','clang'));cxx=shlex.split(os.getenv('CXX','clang++'));rows=[];pins={}
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for short in (False,True):
 d=out/('short' if short else 'default');d.mkdir();objects=[];variant=['-fshort-enums'] if short else []
 for src in (proto/'nanopb/pb_common.c',proto/'nanopb/pb_encode.c',proto/'nanopb/pb_decode.c'):
  obj=d/(src.stem+'.o');cmd=cc+['-std=c11']+flags+variant+inc+['-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True,capture_output=True);objects.append(str(obj));pins[str(src)]=sha(src)
 for name in ('environment_metrics_adapter','environment_query','wall_clock_gnss'):
  src=app/'tests'/(name+'_tests.cpp');exe=d/name
  dep=subprocess.run(cxx+['-std=c++17','-MM']+inc+[str(src)],capture_output=True,text=True,check=True).stdout
  for n in shlex.split(dep.replace('\\\n',' ').split(':',1)[1]):pins[n]=sha(Path(n))
  cmd=cxx+['-std=c++17','-Wall','-Wextra','-Werror']+flags+variant+inc+[str(src),str(app/'src/Protobuf.cpp')]+objects+['-o',str(exe)]
  r=subprocess.run(cmd,capture_output=True,text=True);(d/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
  r=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(d/(name+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();rows.append({'name':name,'short_enums':short,'command':cmd,'returncode':r.returncode})
pins[str(app/'src/Protobuf.cpp')]=sha(app/'src/Protobuf.cpp');pins[str(Path(__file__).resolve())]=sha(Path(__file__).resolve())
assert all(sha(Path(n))==h for n,h in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'hardware':False,'RF':False,'source_inputs_stable':True,'source_pins':pins,'cases':rows},sort_keys=True,indent=2)+'\n');print('PASS six actual native environment/clock executions, both enum modes')
