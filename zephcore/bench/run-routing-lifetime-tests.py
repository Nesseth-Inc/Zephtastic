#!/usr/bin/env python3
"""Source-bound dynamic RoutingCore sanitizer fixture; no radio or transport."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
app=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(app.parent));import host_test_toolchain as host
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
source=app/'bench/tests/routing_dynamic_lifetimes.cpp'
policies=[source,app/'adapters/bench/vendor/RoutingCore.h',app/'adapters/bench/vendor/RadioPacketTiming.h',app/'adapters/bench/vendor/RelayCore.h',app/'adapters/bench/vendor/MtCodec.h',Path(__file__).resolve()]
before={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in policies}
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(app/'adapters/bench/vendor'),*(['-fshort-enums']if a.short_enums else[]),str(source),'-o',str(out/'routing-lifetimes')]
(out/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
with(out/'compile.log').open('x')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
r=subprocess.run([str(out/'routing-lifetimes')],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'run.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
assert sum(x.startswith('PASS ')for x in r.stdout.splitlines())==16
assert before=={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in policies}
(out/'results.json').write_text(json.dumps({'status':'PASS','groups':16,'ASAN_UBSAN':True,'short_enums':a.short_enums,'source_sha256':before,'hardware':False,'RF':False},indent=2)+'\n')
