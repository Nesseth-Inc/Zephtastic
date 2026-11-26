#!/usr/bin/env python3
"""Execute returned RAM-only proposal fixtures against the integrated headers."""
from pathlib import Path
import argparse, hashlib, json, os, shlex, subprocess
if not __debug__:
    raise RuntimeError('Optimized fixture refused')
app=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--short-enums',action='store_true')
a=p.parse_args();out=a.output.resolve()
if out.exists() or out==app.parent or app.parent in out.parents:
    p.error('Fresh output outside source required')
names=('pending_bluetooth_admin_qwen','ble_reply_progress_qwen')
files=[Path(__file__).resolve(),*(app/'tests'/(n+'.cpp') for n in names),
       *(app/'src'/n for n in ('PendingBluetoothAdmin.h','BluetoothSettings.h','BleReplyProgress.h'))]
def pins():
    return {str(f.relative_to(app.parent)):{'bytes':f.stat().st_size,
            'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in files}
before=pins();out.mkdir(mode=0o700,parents=True);results=[]
for name in names:
    command=[*shlex.split(os.environ.get('CXX','c++')),'-std=c++11','-Wall','-Wextra','-Werror',
             '-fno-exceptions','-fno-rtti','-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-I'+str(app/'src'),str(app/'tests'/(name+'.cpp')),'-o',str(out/name)]
    if a.short_enums: command.insert(1,'-fshort-enums')
    compiled=subprocess.run(command,capture_output=True,timeout=60)
    (out/(name+'-compile.log')).write_bytes(compiled.stdout+compiled.stderr)
    ran=None
    if not compiled.returncode:
        ran=subprocess.run([str(out/name)],capture_output=True,timeout=30)
        (out/(name+'-run.log')).write_bytes(ran.stdout+ran.stderr)
    results.append({'name':name,'command':command,'compile_returncode':compiled.returncode,
                    'run_returncode':None if ran is None else ran.returncode})
stable=before==pins();ok=stable and all(r['compile_returncode']==0 and r['run_returncode']==0 for r in results)
result={'status':'PASS_PURE_BLE_ADMIN_PRIMITIVES' if ok else 'FAIL','files':before,
        'source_stable':stable,'results':results,'short_enums':a.short_enums,'ASAN_UBSAN':True,
        'worker_test_claims_credited':False,'hardware_or_transport_qualification':False}
(out/'RESULT.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(result['status']);raise SystemExit(0 if ok else 1)
