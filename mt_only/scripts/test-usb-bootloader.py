#!/usr/bin/env python3
"""Compile the actual USB bootloader adapter with hardware-boundary mocks."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

parser=argparse.ArgumentParser()
parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(app.parent));import host_test_toolchain as host
commands=[];results=[]
for target,cases in [('t096',['ordinary','read_error','touch','error_then_touch','latched','wrap','long_uptime']),('other',['non_t096'])]:
    exe=out/target
    cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
         '-I'+str(app/'src'),'-I'+str(app/'tests/usb_bootloader_stubs'),
         str(app/'tests/usb_bootloader_tests.cpp'),str(app/'src/UsbBootloader.cpp'),'-o',str(exe)]
    if target=='t096':cmd+=['-DCONFIG_BOARD_HELTEC_T096=1']
    subprocess.run(cmd,check=True);commands.append(cmd)
    for case in cases:
        run=subprocess.run([str(exe),case],capture_output=True,text=True,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
        (out/(case+'.log')).write_text(run.stdout+run.stderr)
        print(run.stdout+run.stderr,end='');run.check_returncode()
        results.append({'case':case,'status':'PASS'})
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'actual_adapter_compiled':True,
                                          'hardware':False,'scenarios':results},indent=2)+'\n')
