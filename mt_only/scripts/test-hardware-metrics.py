#!/usr/bin/env python3
"""Native proposal tests. Reads selected actual production source; writes only --output.
Run default and short enums with C11 mocks, C++17, Werror, ASAN and UBSAN.
"""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
if not __debug__:raise RuntimeError('Optimized hardware qualification refused')
sys.dont_write_bytecode=True
SCRIPT=Path(__file__).resolve()
APP=SCRIPT.parents[1]
HERE=APP/'tests/hardware_metrics'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=APP.parent)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args();root=a.source.resolve();out=a.output.resolve()
if out==root or root in out.parents:p.error('--output must be outside the source tree')
if out.exists():p.error('--output must not already exist; preserve failed attempts')
out.mkdir(parents=True)
app=root/'mt_only';proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';helpers=root/'zephcore/helpers'
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
commands=[];results=[]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def state():
 files=[v for folder in (app/'src',app/'tests',vendor,mono,proto,helpers) for v in folder.rglob('*') if v.is_file() and v.suffix in ('.h','.c','.cpp','.inc')]
 return {str(v.relative_to(root)):digest(v) for v in sorted(files)}
before=state();(out/'source-sha256-before.json').write_text(json.dumps(before,indent=2)+'\n')
def run(cmd,log,execute=False):
 commands.append(cmd);r=subprocess.run(cmd,text=True,capture_output=True,timeout=120,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','PYTHONDONTWRITEBYTECODE':'1'})
 (out/log).write_text(r.stdout+r.stderr)
 if execute:
  cases=[s for s in r.stdout.splitlines() if s.startswith('PASS ')]
  results.append({'command':cmd,'exit_code':r.returncode,'cases':cases,'log':log})
 if r.returncode:
  print(r.stdout+r.stderr);raise RuntimeError('Command failed; preserved '+str(out/log))
 if execute:print(r.stdout,end='')
 return r
try:
 (out/'meshtastic').mkdir()
 for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
 base=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O2']
 includes=['-I'+str(v) for v in (app/'src',app/'tests',vendor,mono,out,proto/'nanopb',helpers)]
 for short in (False,True):
  mode='short' if short else 'default';folder=out/mode;folder.mkdir()
  flags=base+(['-fshort-enums'] if short else [])
  curve=folder/'battery_curve.o';run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(helpers),'-c',str(helpers/'battery_curve.c'),'-o',str(curve)],mode+'/curve-build.log')
  curve_exe=folder/'battery-curve-tests'
  run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(helpers),str(HERE/'battery_curve_cases.c'),str(curve),'-o',str(curve_exe)],mode+'/battery-curve-fixture-build.log')
  run([str(curve_exe)],mode+'/battery-curve-fixture.log',True)
  for config,defs,scenarios in (
   ('t096',['-DMT_MOCK_HAS_ADC=1','-DCONFIG_SOC_SERIES_NRF52=1'],['adc_not_ready','divider_not_ready','setup_error','success_and_cadence','adc_divider_override','enable_error_clears_previous','sequence_error','read_error','negative_sample','overflow_sample','disable_error_stops_attempts','raw_and_envelope_boundaries','freshness_and_clock','usb_independent']),
   ('no-divider',['-DMT_MOCK_HAS_ADC=1','-DCONFIG_SOC_SERIES_NRF52=1','-DMT_MOCK_NO_DIVIDER=1'],['no_divider']),
   ('xiao',['-DMT_MOCK_HAS_ADC=0'],['unsupported'])):
   mockobj=folder/(config+'-mock.o');mockflags=['-I'+str(HERE/'mocks'),'-I'+str(HERE)]
   run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,*defs,*mockflags,'-c',str(HERE/'mock_api.c'),'-o',str(mockobj)],mode+'/'+config+'-mock-build.log')
   exe=folder/(config+'-hardware-tests')
   run([*cxx,'-std=c++17','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Werror',*flags,*defs,*mockflags,*includes,str(HERE/'hardware_cases.cpp'),str(app/'src/ZephyrHardware.cpp'),str(mockobj),str(curve),'-o',str(exe)],mode+'/'+config+'-hardware-build.log')
   for scenario in scenarios:run([str(exe),scenario],mode+'/'+config+'-'+scenario+'.log',True)
  objs=[]
  for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
   obj=folder/(src.stem+'.o');run([*cc,'-std=c11',*flags,*includes,'-c',str(src),'-o',str(obj)],mode+'/'+src.stem+'-build.log');objs.append(str(obj))
  host=['-DMT_HOST_OPENSSL=1'] if sys.platform.startswith('linux') else []
  exe=folder/'producer-tests';cmd=[*cxx,'-std=c++17','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Werror',*flags,*host,*includes,str(HERE/'producer_cases.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objs]
  if sys.platform.startswith('linux'):cmd+=['-lcrypto']
  cmd+=['-o',str(exe)];run(cmd,mode+'/producer-build.log');run([str(exe)],mode+'/producer-tests.log',True)
 status='PASS';error=None
except (OSError,subprocess.SubprocessError,RuntimeError) as exc:status='FAIL';error=str(exc)
after=state();stable=before==after
if not stable:status='FAIL_SOURCE_CHANGED'
record={'status':status,'error':error,'hardware':False,'RF':False,'runtime_credentials_or_NVS':False,'source_stable':stable,'ASAN_UBSAN':True,'optimization':'O2','C_mocks':True,'CXX_standard':'c++17','warnings':'Wall Wextra Werror','enum_matrix':['default','fshort-enums'],'actual_hardware_source':str(app/'src/ZephyrHardware.cpp'),'actual_curve_source':str(helpers/'battery_curve.c'),'producer_limit':'Private Endpoint producer encoder is real; authenticated transport is outside this harness. No physical ADC/USB/GNSS proof.','scenario_runs':results,'pass_lines':sum(len(x['cases']) for x in results),'fixture_sha256':{str(f.relative_to(APP)):digest(f) for f in sorted([*(HERE/'mocks').rglob('*.h'),SCRIPT,*(HERE/n for n in ('mock_control.h','mock_api.c','hardware_cases.cpp','producer_cases.cpp','battery_curve_cases.c'))])}}
(out/'source-sha256-after.json').write_text(json.dumps(after,indent=2)+'\n');(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'RESULTS.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'status':status,'source_stable':stable,'pass_lines':record['pass_lines'],'error':error,'output':str(out)}))
sys.exit(0 if status=='PASS' else 1)
