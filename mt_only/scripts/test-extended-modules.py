#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""M112 actual module-extension codec and journal sanitizer tests."""
if not __debug__:raise RuntimeError('optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
p.add_argument('--only',nargs='+',help='Run selected registered cases for a focused change')
a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve()
if out.exists()or out.is_relative_to(root):p.error('fresh output outside source required')
out.mkdir(parents=True);proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
(out/'meshtastic').mkdir()
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
sha=lambda b:hashlib.sha256(b).hexdigest()
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
flags=['-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];link=['-lcrypto']
tests=['extended_module_settings_tests','same_channel_beacon_payload_tests','mesh_beacon_received_cache_tests','stock_position_dedup_tests','unknown_packet_window_tests','stock_per_source_rate_window_tests','traffic_service_tests','traffic_role_lifecycle_tests','traffic_remove_lifecycle_tests','traffic_management_window_tests','beacon_cadence_tests','nodeinfo_payload_cache_tests','beacon_controller_tests']
tests+=['local_input_tests','public_application_cases']
tests+=['zephyr_settings_diagnostic_tests']
if a.only:
 if len(set(a.only))!=len(a.only)or any(name not in tests for name in a.only):p.error('Unknown or duplicate selected test')
 tests=a.only
common=[mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c']],app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp']
pins={};runs=[];result={'status':'FAIL','ASAN_UBSAN':True,'hardware':False,'network':False,'executions':runs}
def execute(command,log,timeout=90):
 log.write(shlex.join(command)+'\n');log.flush();subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
def compile_source(src,obj,log,extra):
 c=cc if src.suffix=='.c'else cxx;language=['-std=c11']if src.suffix=='.c'else['-std=c++17','-Wall','-Wextra','-Werror']
 dep=obj.with_suffix('.d');execute([*c,*language,*flags,*extra,'-MM','-MT','x','-MF',str(dep),str(src)],log)
 dependencies=[Path(n)for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])]
 before={str(n):sha(n.read_bytes())for n in dependencies};execute([*c,*language,*flags,*extra,'-c',str(src),'-o',str(obj)],log)
 assert all(sha(Path(n).read_bytes())==s for n,s in before.items());pins.update(before)
try:
 for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
  folder=out/label;folder.mkdir();objects=[]
  with(folder/'build.log').open('x')as log:
   for i,s in enumerate(common):
    obj=folder/('common-'+str(i)+'.o');compile_source(s,obj,log,extra);objects.append(str(obj))
   for name in tests:
    selected_extra=extra+(['-DCONFIG_MT_TCP_COMPANION=1']if name=='tcp_owner_tests'else[])
    if name=='zephyr_settings_diagnostic_tests':selected_extra+=['-I'+str(app/'tests'/'stubs')]
    case_objects=[]
    if name=='beacon_controller_tests':
     selected_extra+=['-I'+str(app/'tests/beacon/stubs')]
     for unit in ['BeaconTargetRuntime','BeaconExcursion']:
      obj=folder/(unit+'.o')
      compile_source(app/'src/modules'/(unit+'.cpp'),obj,log,selected_extra)
      case_objects.append(str(obj))
    src=app/'tests'/(name+'.cpp');obj=folder/(name+'.o');compile_source(src,obj,log,selected_extra)
    binary=folder/name;execute([*cxx,*flags,*extra,str(obj),*case_objects,*objects,*link,'-o',str(binary)],log)
    with(folder/(name+'.log')).open('x')as testlog:
     r=subprocess.run([str(binary)],stdout=testlog,stderr=subprocess.STDOUT,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
    assert r.returncode==0,(name,label,r.returncode)
    runs.append({'test':name,'variant':label,'TCP_owner_count':3 if name=='tcp_owner_tests'else 2,'exit':r.returncode,'binary_sha256':sha(binary.read_bytes()),'log_sha256':sha((folder/(name+'.log')).read_bytes())})
 assert len(runs)==2*len(tests) and all(sha(Path(n).read_bytes())==s for n,s in pins.items())
 result.update(status='PASS',source_stable=True,compiled_input_sha256=pins)
finally:(out/'RESULT.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':result['status'],'executions':len(runs),'ASAN_UBSAN':True,'hardware':False,'network':False}))
