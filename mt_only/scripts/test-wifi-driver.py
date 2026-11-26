#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Execute actual copied driver callbacks with explicit native API stand-ins."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess
if not __debug__:raise RuntimeError('optimized tests refused')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();app=Path(__file__).resolve().parents[1];root=app.parent
if out.exists()or out.is_relative_to(root):p.error('fresh external output required')
out.mkdir(parents=True);includes=['kernel.h','net/net_if.h','net/net_event.h','net/net_mgmt.h','net/wifi_mgmt.h','net/net_ip.h','net/sntp.h','logging/log.h','sys/atomic.h']
for n in includes:
 f=out/'include/zephyr'/n;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#pragma once\n')
inputs=[app/'tests/wifi_lease_status_tests.cpp',app/'src/network/WifiRuntime.cpp',app/'src/network/WifiRuntime.h',Path(__file__).resolve(),app/'tests/wifi_driver_event_tests.c',app/'tests/wifi_native_api.h',root/'zephcore/adapters/wifi/ZephyrWiFiStation.c',root/'zephcore/adapters/wifi/ZephyrWiFiStation.h',root/'zephcore/helpers/pm_sleep_guard.h'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();pins={str(p):sha(p)for p in inputs}
cmd=[*shlex.split(os.environ.get('CC','clang')),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(out/'include'),'-include',str(app/'tests/wifi_native_api.h'),str(app/'tests/wifi_driver_event_tests.c'),'-o',str(out/'tests')]
cxx=[*shlex.split(os.environ.get('CXX','clang++')),'-std=c++17','-fsanitize=address,undefined','-g']
for folder in (out/'include',app/'src',root/'zephcore/adapters/wifi',root/'zephcore/adapters/bench/vendor',root/'zephcore/lib/monocypher'):
 cxx.append('-I'+str(folder))
cxx.extend([str(app/'tests/wifi_lease_status_tests.cpp'),'-o',str(out/'lease-status')])
result={'status':'FAIL','hardware':False,'network':False,'ASAN_UBSAN':True,'actual_driver_body':True,'Zephyr_ABI_or_scheduler_qualified':False,'source_sha256':pins,'command':cmd,'lease_command':cxx}
try:
 with(out/'build.log').open('x')as f:subprocess.run(cmd,check=True,stdout=f,stderr=subprocess.STDOUT,timeout=60)
 r=subprocess.run([str(out/'tests')],capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'},timeout=30);(out/'test.log').write_bytes(r.stdout+r.stderr);assert r.returncode==0
 with(out/'lease-build.log').open('x')as f:subprocess.run(cxx,check=True,stdout=f,stderr=subprocess.STDOUT,timeout=60)
 lease=subprocess.run([str(out/'lease-status')],capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'},timeout=30)
 (out/'lease-test.log').write_bytes(lease.stdout+lease.stderr);assert lease.returncode==0 and lease.stdout.startswith(b'PASS ')
 assert sum(s.startswith(b'PASS ')for s in r.stdout.splitlines())==8 and all(sha(Path(n))==s for n,s in pins.items());result.update(status='PASS',groups=9,source_stable=True)
finally:(out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print(json.dumps({k:result[k]for k in ['status','groups','ASAN_UBSAN','actual_driver_body','Zephyr_ABI_or_scheduler_qualified','network','hardware']}))
