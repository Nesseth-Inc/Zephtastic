#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Memory-only module settings/journal adversaries; no device or network."""
if not __debug__:raise RuntimeError('Optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve()
if out.exists()or out.is_relative_to(root):p.error('fresh output outside source required')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
inputs=[app/'src'/n for n in ('ModuleSettings.h','ModuleJournal.h','Config.h','SettingsJournal.h','PrimaryChannel.hpp')]
inputs += list(vendor.glob('*.h'))+list(proto.glob('*.pb.h'))+list((proto/'nanopb').glob('*.h'))+[app/'tests/module_settings_cases.cpp',Path(__file__).resolve()]
sha=lambda b:hashlib.sha256(b).hexdigest()
def pins():return {str(path.relative_to(root)):sha(path.read_bytes())for path in inputs}
before=pins();out.mkdir(parents=True);(out/'meshtastic').mkdir()
for path in proto.glob('*.pb.h'):shutil.copyfile(path,out/'meshtastic'/path.name)
flags=['-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
cxx=shlex.split(os.environ.get('CXX','clang++'));rows=[];result={'status':'FAIL','hardware':False,'network':False,'ASAN_UBSAN':True,'source_sha256':before,'variants':rows}
try:
 for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
  cmd=[*cxx,*flags,*extra,str(app/'tests/module_settings_cases.cpp'),'-o',str(out/label)]
  with(out/(label+'.build.log')).open('x')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
  with(out/(label+'.test.log')).open('x')as log:subprocess.run([str(out/label)],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  lines=(out/(label+'.test.log')).read_text().splitlines();assert sum(line.startswith('PASS ')for line in lines)==10 and lines[-1]=='MODULE_SETTINGS_GROUPS 10'
  rows.append({'variant':label,'groups':10,'command':cmd,'binary_sha256':sha((out/label).read_bytes())})
 assert pins()==before;result.update(status='PASS',source_stable=True)
finally:(out/'results.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':result['status'],'variants':len(rows),'groups_per_variant':10}))
