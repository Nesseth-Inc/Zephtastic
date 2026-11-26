#!/usr/bin/env python3
"""Run actual BLE callback bodies against bounded stack doubles, never hardware."""
from pathlib import Path
import argparse, hashlib, json, os, shlex, subprocess

app=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--stock-identity-import', action='store_true', help='Exercise the ordinary XIAO authenticated import transport')
p.add_argument('--short-enums',action='store_true')
p.add_argument('--diagnostics',action='store_true')
a=p.parse_args()
out=a.output.resolve()
if out.exists() or out==app.parent or app.parent in out.parents:
    p.error('Use a fresh output outside the source tree')
paths=[Path(__file__).resolve(),app/'tests/ble_runtime_cases.cpp']
paths += sorted((app/'tests/ble_runtime').rglob('*.h'))
paths += [app/'src'/s for s in ('BleTransport.cpp','BleTransport.h','BleDiagnostics.h','BluetoothPairingPolicy.h','BluetoothSettings.h','BlePairingEvent.h','BleReplyProgress.h')]
def pins():
    return {s.relative_to(app.parent).as_posix():{'bytes':s.stat().st_size,'sha256':hashlib.sha256(s.read_bytes()).hexdigest()}for s in paths}
before=pins()
out.mkdir(parents=True)
command=[*shlex.split(os.environ.get('CXX','c++')),'-std=c++17','-Wall','-Wextra','-Werror',
         '-Wno-unused-function','-Wno-unused-variable','-Wno-missing-field-initializers',
         '-fsanitize=address,undefined','-g','-DCONFIG_MT_DISPLAY=1','-DCONFIG_BT_APP_PASSKEY=1',
         '-I'+str(app/'tests/ble_runtime/stubs'),'-I'+str(app/'tests/ble_runtime'),'-I'+str(app/'src')]
if a.stock_identity_import:command+=['-DCONFIG_MT_STOCK_IDENTITY_IMPORT=1']
if a.short_enums:command+=['-fshort-enums']
if a.diagnostics:command+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
command += [str(app/'tests/ble_runtime_cases.cpp'),'-o',str(out/'cases')]
with(out/'compile.log').open('w')as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
with(out/'test.log').open('w')as log:
    subprocess.run([str(out/'cases')],stdout=log,stderr=subprocess.STDOUT,check=True,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
assert before==pins(),'Source changed during validation'
r={'status':'PASS_ACTUAL_BLE_CALLBACK_BODIES_WITH_STACK_DOUBLES','source':before,'command':command,
   'stock_identity_import':a.stock_identity_import,'short_enums':a.short_enums,'diagnostics':a.diagnostics,'ASAN_UBSAN':True,
   'actual_Zephyr_stack_or_SMP_crypto':False,'hardware_or_physical_pairing':False,
   'thread_concurrency_or_deadlock_proof':False,'blocking_stack_calls_outside_mock_session_lock':True}
r['admin_barrier_cases']=['secured-dispatch-admission','ordinary-queue-and-read-blob-purge',
    'write-prepare-send-dispatch-exclusion','single-canonical-result-and-token-binding',
    'contiguous-repeat-overlap-read-coverage','notify-and-CCC-do-not-drain-or-authorize',
    'fixed-64-bit-deadline-and-first-drain-grace','clock-regression-fail-closed',
    'missing-or-disallowed-reply-policy-handoff','disconnect-and-stale-reconnect',
    'security-loss-latches-closed','settings-authority-loss','explicit-policy-override']
(out/'RESULT.json').write_text(json.dumps(r,sort_keys=True,indent=2)+'\n')
print(json.dumps({k:r[k]for k in ('status','short_enums','diagnostics','ASAN_UBSAN')}))
