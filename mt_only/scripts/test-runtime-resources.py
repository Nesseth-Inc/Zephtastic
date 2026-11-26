#!/usr/bin/env python3
import argparse,pathlib,subprocess,os,json,hashlib
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=pathlib.Path);a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False);app=pathlib.Path(__file__).resolve().parents[1];rows=[]
for label,flags in [('enabled',['-DCONFIG_SYS_HEAP_RUNTIME_STATS=1','-DCONFIG_HEAP_MEM_POOL_SIZE=16384']),('no-heap',['-DCONFIG_SYS_HEAP_RUNTIME_STATS=1','-DCONFIG_HEAP_MEM_POOL_SIZE=0']),('no-stats',['-DCONFIG_HEAP_MEM_POOL_SIZE=16384'])]:
 exe=out/label;cmd=['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie','-I'+str(app/'tests/runtime-platform'),'-I'+str(app/'src'),*flags,str(app/'tests/runtime_resources_tests.cpp'),'-o',str(exe)]
 subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0;rrow={'profile':label,'status':'PASS','command':cmd};rows.append(rrow)
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','hardware':False,'ASAN_UBSAN':True,'profiles':rows},indent=2));print('PASS all three runtime profiles')
