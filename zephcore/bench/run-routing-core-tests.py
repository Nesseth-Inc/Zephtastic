#!/usr/bin/env python3
"""Offline bounded routing/native-gate checks under ASAN/UBSAN."""
import argparse,json,os,subprocess,sys
from pathlib import Path
app=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(app.parent));import host_test_toolchain as host
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--t096',action='store_true');args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);results=[]
for name in ['routing_core_tests','routing_gate_tests','routing_bounds_tests']:
    binary=out/name;cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        *(['-DCONFIG_BOARD_HELTEC_T096=1'] if args.t096 else []),str(app/'bench/tests'/f'{name}.cpp'),'-o',str(binary)]
    subprocess.run(cmd,check=True)
    (out/(name+'-compile.json')).write_text(json.dumps(cmd,indent=2)+'\n')
    with (out/(name+'.log')).open('w') as log:
        p=subprocess.run([str(binary)],stdout=log,stderr=subprocess.STDOUT,timeout=60,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    assert p.returncode==0,(name,p.returncode);results.append({'test':name,'exit':p.returncode})
    print(name+': PASS',flush=True)
(out/'results.json').write_text(json.dumps({'scenarios':results,'t096':args.t096,'ASan_UBSan':True,'hardware':False},indent=2)+'\n')
