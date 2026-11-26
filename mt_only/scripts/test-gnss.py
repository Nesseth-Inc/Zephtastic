#!/usr/bin/env python3
"""GNSS state/projection and saved-config migration; native only, no hardware."""
import argparse,datetime,hashlib,json,os,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();out.mkdir(exist_ok=False)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';fixtures=app/'tests/gnss/fixtures'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
provenance=json.loads((fixtures/'PROVENANCE.json').read_text())
assert provenance['synthetic'] and not provenance['operating_identity_or_keys']
for n,row in provenance['files'].items():
 assert n in {'v14-config.bin','v14-journal.bin','v15-config.bin','v15-journal.bin'}
 assert (fixtures/n).stat().st_size==row['bytes'] and sha(fixtures/n)==row['sha256']
assert set(provenance['files'])=={'v14-config.bin','v14-journal.bin','v15-config.bin','v15-journal.bin'}
(out/'meshtastic').mkdir()
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
compiler=shlex.split(os.environ.get('CXX','clang++'));rows=[]
inputs=[Path(__file__).resolve(),*fixtures.glob('*'),*proto.glob('*.pb.h')]
pins={str(p):sha(p)for p in inputs}
for short in (False,True):
 directory=out/('short'if short else'default');directory.mkdir()
 flags=['-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']+(['-fshort-enums']if short else[])
 includes=['-I'+str(v)for v in [app/'src',vendor,root/'zephcore/lib/monocypher',out,proto/'nanopb']]
 for label in ['gnss_state','gnss_projection','gnss_config','gnss_utc','gnss_units','gnss_measured','gnss_clock','gnss_capability_absent','gnss_capability_fitted','gnss_capability_xiao']:
  src=app/'tests'/('gnss_capability_tests.cpp'if label.startswith('gnss_capability')else label+'_tests.cpp');exe=directory/label
  dep=subprocess.run(compiler+['-std=c++17','-MM',*includes,str(src)],capture_output=True,text=True,check=True,timeout=60).stdout
  names=shlex.split(dep.replace('\\\n',' ').split(':',1)[1]);pins.update({n:sha(Path(n))for n in names})
  command=compiler+flags+(['-DCONFIG_MT_GNSS_T096=1']if label=='gnss_capability_fitted'else ['-DCONFIG_MT_GNSS_XIAO_L76K=1']if label=='gnss_capability_xiao'else [])+includes+[str(src),'-o',str(exe)]
  compiled=subprocess.run(command,capture_output=True,text=True,timeout=120);(directory/(label+'-compile.log')).write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
  argv=[str(exe)]+([str(fixtures)]if label=='gnss_config'else[])
  result=subprocess.run(argv,capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  (directory/(label+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
  rows.append({'label':label,'short_enums':short,'ASAN_UBSAN':True,'command':command,'run':argv,'returncode':result.returncode,'literal_PASS_lines':[l for l in result.stdout.splitlines()if l.startswith('PASS ')]})
calendar=out/'utc-calendar.cpp';calendar.write_text('''#include "gnss/GnssUtcTime.h"
#include <cstdio>
int main(){unsigned y,m,d,h,n,ms;while(scanf("%u %u %u %u %u %u",&y,&m,&d,&h,&n,&ms)==6){uint32_t out=0;const bool ok=zt::gnss::toUnixSeconds({uint8_t(y),uint8_t(m),uint8_t(d),uint8_t(h),uint8_t(n),uint16_t(ms)},out);printf("%u %u\\n",unsigned(ok),unsigned(out));}}
''')
calendar_exe=out/'utc-calendar';cmd=compiler+flags+includes+[str(calendar),'-o',str(calendar_exe)]
r=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(out/'utc-calendar-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
inputs=[];expected=[];date=datetime.date(2000,1,1)
while date<datetime.date(2100,1,1):
 for ms in (0,999,1000,59999):
  inputs.append(f'{date.year-2000} {date.month} {date.day} 12 34 {ms}\n')
  expected.append(f'1 {int(datetime.datetime(date.year,date.month,date.day,12,34,ms//1000,tzinfo=datetime.timezone.utc).timestamp())}\n')
 date+=datetime.timedelta(days=1)
r=subprocess.run([str(calendar_exe)],input=''.join(inputs),capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});r.check_returncode()
assert r.stdout==''.join(expected),'UTC differs from Python datetime reference'
assert all(sha(Path(n))==h for n,h in pins.items()),'compiled input changed during native checks'
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','hardware':False,'RF':False,'firmware_build':False,'legacy_fixtures_are_synthetic':True,'source_inputs_stable':True,'source_pins':pins,'cases':rows,'utc_calendar_reference':{'source':'Python datetime UTC, all2000..2099 dates and4within-minute values','cases':len(expected),'command':cmd,'reference_output_sha256':hashlib.sha256(r.stdout.encode()).hexdigest()}},indent=2,sort_keys=True)+'\n')
print('PASS '+str(len(rows))+' native executions: GNSS state/projection/config and stable fitted/absent capability; both enum modes')
