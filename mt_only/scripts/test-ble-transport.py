#!/usr/bin/env python3
"""Test actual BLE callbacks and session races with host stand-ins; no devices."""
import argparse,json,os,subprocess,re,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--out-queue-depth',type=int,choices=(8,32,64),default=64);a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(app.parent));import host_test_toolchain as host
for name in ('kernel.h','bluetooth/bluetooth.h','bluetooth/conn.h','bluetooth/gatt.h','sys/atomic.h','sys/byteorder.h'):
 h=out/'zephyr'/name;h.parent.mkdir(parents=True,exist_ok=True);h.write_text('#include "ble_zephyr_mock.h"\n')
commands=[]
def build(command,name):
 commands.append(command)
 result=subprocess.run(command,capture_output=True,text=True)
 (out/(name+'.build.log')).write_text(result.stdout+result.stderr)
 if result.returncode:print(result.stdout+result.stderr)
 result.check_returncode()
cpp_flags=['-DCONFIG_MT_DISPLAY=1','-DCONFIG_BT_APP_PASSKEY=1',f'-DMT_BLE_OUT_QUEUE_DEPTH={a.out_queue_depth}']
for name in ('ble_transport_tests','ble_session_race_tests','ble_security_timing_tests'):
 cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-pthread',*cpp_flags,'-I'+str(out),'-I'+str(app/'tests'),str(app/('tests/'+name+'.cpp')),'-o',str(out/name)]
 build(cmd,name)
 r=subprocess.run([str(out/name)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},timeout=30)
 (out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
# Default-off executables must contain no optional observer hooks or diagnostic
# state. Pairing completion/failure callbacks now clear the RAM pairing event in
# every build, independently of diagnostics.
symbols=subprocess.check_output(['nm',str(out/'ble_transport_tests')],text=True)
assert not re.search(r'mt_ble_trace_|__wrap_bt_l2cap_|L10diagnosticE|11diagnostics',symbols)
# Compile the real observer as C, matching both firmware architectures' C ABI.
croot=out/'smp-c';croot.mkdir(exist_ok=True)
for name in ('zephyr/bluetooth/conn.h','zephyr/bluetooth/l2cap.h','zephyr/sys/byteorder.h','conn_internal.h','l2cap_internal.h'):
 h=croot/name;h.parent.mkdir(parents=True,exist_ok=True);h.write_text('#include "ble_smp_c_mock.h"\n')
cc=[*host.cc(),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(croot),'-I'+str(app/'tests'),'-c',str(app/'src/BleSmpTrace.c'),'-o',str(out/'smp-trace.o')]
build(cc,'smp-trace')
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-pthread',*cpp_flags,'-I'+str(out),'-I'+str(app/'tests'),str(app/'tests/ble_diagnostic_tests.cpp'),str(out/'smp-trace.o'),'-o',str(out/'ble_diagnostic_tests')]
build(cmd,'ble_diagnostic_tests')
r=subprocess.run([str(out/'ble_diagnostic_tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},timeout=30)
(out/'ble_diagnostic_tests.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
(out/'results.json').write_text(json.dumps({'status':'PASS','scenario_groups':11,'ASAN_UBSAN':True,'out_queue_depth':a.out_queue_depth,'hardware':False,'limitations':'Host mutex and queue stand-ins; no pairing, ATT reassembly, physical MTU negotiation or Zephyr scheduler qualification','build_commands':commands},indent=2)+'\n')
