#!/usr/bin/env python3
"""Compare original M60 and instrumented Service radio-call traces; no hardware."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;sys.path.insert(0,str(root));import host_test_toolchain as host
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source'
if (out/'results.json').exists():raise RuntimeError('Preserve prior trace result')
baseline=app/'tests/storage_diag_baseline_Service.h'
original_baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest()
if original_baseline_sha256!='7a99774ab45f34612bd146f3a2b8976e0279644aee51609b283ad3c7423a2773':raise RuntimeError('Original M60 Service fixture changed')
(out/'baseline').mkdir();baseline_copy=out/'baseline/Service.h';shutil.copyfile(baseline,baseline_copy)
old_call=b'endpoint_.receive(codec,route.identity(),p,n,m,snr,now,random,endpointArmed_&&txAllowed_);'
new_call=b'endpoint_.receive(codec,route,route.identity(),p,n,m,snr,now,random,endpointArmed_&&txAllowed_);'
baseline_copy_bytes=baseline_copy.read_bytes()
if baseline_copy_bytes.count(old_call)!=1:raise RuntimeError('Expected exactly one frozen M60 Endpoint.receive call in temporary baseline copy')
baseline_copy_bytes=baseline_copy_bytes.replace(old_call,new_call,1)
baseline_copy.write_bytes(baseline_copy_bytes)
transformed_baseline_sha256=hashlib.sha256(baseline_copy_bytes).hexdigest()
(out/'baseline-transform.json').write_text(json.dumps({'source_path':str(baseline),'original_sha256':original_baseline_sha256,
 'temporary_copy_path':str(baseline_copy),'temporary_copy_sha256':transformed_baseline_sha256,
 'exact_replacements':1,'old_call':old_call.decode(),'new_call':new_call.decode(),
 'scope':'Compile-only compatibility seam; trace test does not invoke Endpoint.receive.'},indent=2)+'\n')
(out/'meshtastic').mkdir()
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
common=['-fsanitize=address,undefined','-g','-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),*host.crypto_compile_flags()]
if a.short_enums:common+=['-fshort-enums']
objects=[];commands=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*host.cc(),'-std=c11',*common,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);objects.append(str(obj));commands.append(cmd)
logs={}
for name,head,defines in [('baseline',out/'baseline',[]),('instrumented',app/'src',['-DSTORAGE_DIAGNOSTICS_TEST=1'])]:
 exe=out/(name+'-traces');cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-I'+str(head),'-I'+str(app/'src'),*common,*defines,str(app/'tests/service_pause_trace_tests.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host.crypto_link_flags(),'-o',str(exe)]
 subprocess.run(cmd,check=True);commands.append(cmd)
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(name+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();logs[name]=r.stdout
if not logs['baseline']or logs['baseline']!=logs['instrumented']:raise RuntimeError('Observable radio-call behavior differs')
rows=len(logs['baseline'].splitlines());print(logs['instrumented'],end='')
v={'status':'PASS','ASAN_UBSAN':True,'short_enums':a.short_enums,'behavioral_lines':rows,'baseline_matches_instrumented':True,'hardware':False,
 'baseline_transform':{'original_sha256':original_baseline_sha256,'temporary_copy_sha256':transformed_baseline_sha256,'exact_replacements':1,
  'old_call':old_call.decode(),'new_call':new_call.decode(),'receive_exercised':False,'scope':'compile-only compatibility seam'},
 'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in [Path(__file__),baseline,app/'src/Service.h',app/'tests/service_pause_trace_tests.cpp']}}
(out/'results.json').write_text(json.dumps(v,indent=2)+'\n');(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');print('PASS original/instrumented Service radio-call traces match')
