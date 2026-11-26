#!/usr/bin/env python3
"""Compile source-owned acceptance cases against a frozen integration.
No hardware. Requires a reviewed adapter.cpp for the actual delivered API.
"""
import argparse,datetime,hashlib,json,os,pathlib,shlex,shutil,subprocess,sys
if not __debug__:raise SystemExit("Refusing disabled assertions")
from bounded_process import run_group,install_termination_handler
install_termination_handler()
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',required=True,type=pathlib.Path)
p.add_argument('--output',required=True,type=pathlib.Path)
p.add_argument('--short-enums',action='store_true')
p.add_argument('--fixture',choices=['accounting','meter-quality','window-math','actual-responder','local-telemetry'],default='accounting')
p.add_argument('--adapter-file',default='adapter.cpp')
p.add_argument('--input-manifest',required=True,type=pathlib.Path)
p.add_argument('--input-manifest-sha256',required=True)
a=p.parse_args();source=a.source.resolve();out=a.output.resolve();here=pathlib.Path(__file__).resolve().parent
if out.is_relative_to(source) or out.is_relative_to(here):p.error('output must be external to both frozen input trees')
adapter=(here/a.adapter_file).resolve()
if adapter.parent!=here or not adapter.is_file():p.error('reviewed adapter file in fixture directory is required; fixture remains UNRUN')

def hashes(root):
 return {str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest()
         for x in root.rglob('*') if x.is_file() and '.git' not in x.parts
         and '__pycache__' not in x.parts}
# Source must be a bounded export/freeze, not a build tree with mutable outputs.
manifest_bytes=a.input_manifest.read_bytes()
if hashlib.sha256(manifest_bytes).hexdigest()!=a.input_manifest_sha256:p.error('reviewed input manifest digest mismatch')
manifest=json.loads(manifest_bytes)
before=hashes(source);independent=hashes(here)
if before!=manifest.get('source_sha256')or independent!=manifest.get('independent_sha256'):p.error('frozen input identity mismatch')
tools={}
for name in ('cc','cxx','python'):
 expected=manifest.get('toolchain',{}).get(name,{})
 tool=pathlib.Path(expected.get('path',''))
 if not tool.is_absolute()or not tool.is_file()or tool.resolve()!=tool:p.error('absolute resolved toolchain path required')
 if hashlib.sha256(tool.read_bytes()).hexdigest()!=expected.get('sha256'):p.error('toolchain digest mismatch')
 tools[name]=tool
if pathlib.Path(sys.executable).resolve()!=tools['python']:p.error('Python interpreter does not match the reviewed toolchain')
out.mkdir(exist_ok=False,parents=True)
receipt={'status':'FAIL','hardware':False,'rf':False,'short_enums':a.short_enums,'fixture':a.fixture,'adapter_file':a.adapter_file,'started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_sha256':before,'independent_sha256':independent,'input_manifest_sha256':a.input_manifest_sha256,'toolchain':manifest['toolchain']}
env={'PATH':'/usr/bin:/bin','LANG':'C','LC_ALL':'C','ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1','PYTHONDONTWRITEBYTECODE':'1'}
commands=[]
try:
 sys.path.insert(0,str(source))
 from host_test_toolchain import crypto_compile_flags,crypto_link_flags
 app=source/'mt_only';vendor=source/'zephcore/adapters/bench/vendor';mono=source/'zephcore/lib/monocypher';proto=source/'zephcore/bench/upstream-differential/source'
 (out/'meshtastic').mkdir()
 for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
 flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-I'+str(here),*crypto_compile_flags()]
 if a.short_enums:flags.append('-fshort-enums')
 compiler={name:manifest['toolchain'][name].get('argv',[str(tools[name])])for name in ('cc','cxx')}
 for name,argv in compiler.items():
  if not argv or pathlib.Path(argv[0]).resolve()!=tools[name]:raise AssertionError('Compiler argv identity mismatch')
 objects=[]
 for path in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
  obj=out/(path.stem+'.o');cmd=[*compiler['cc'],'-std=c11',*flags,'-c',str(path),'-o',str(obj)];commands.append(cmd)
  with(out/(path.stem+'-build.log')).open('w')as log:run_group(cmd,env=env,stdout=log,timeout=180)
  objects.append(str(obj))
 test=here/dict(accounting='service_accounting_tests.cpp',**{'meter-quality':'airtime_quality_tests.cpp','window-math':'test_tx_window.cpp','actual-responder':'actual_responder_bridge.cpp','local-telemetry':'local_telemetry_bridge.cpp'})[a.fixture]
 exe=out/dict(accounting='independent-service-accounting',**{'meter-quality':'independent-meter-quality','window-math':'independent-window-math','actual-responder':'actual-responder-bridge','local-telemetry':'local-telemetry-bridge'})[a.fixture]
 link=[*compiler['cxx'],'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(test),str(adapter),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*crypto_link_flags(),'-o',str(exe)];commands.append(link)
 with(out/'independent-build.log').open('w')as log:run_group(link,env=env,stdout=log,timeout=180)
 receipt['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest()
 receipt['executable']=str(exe)
 if a.fixture in ('actual-responder','local-telemetry'):
  receipt['status']='COMPILED_NOT_RUN'
 else:
  commands.append([str(exe)])
  with (out/'test.log').open('w')as log:run_group([str(exe)],env=env,stdout=log,timeout=30)
  receipt['run_exit']=0;receipt['groups']=sum(x.startswith('PASS ')for x in (out/'test.log').read_text().splitlines())
  if receipt['groups']!=dict(accounting=20,**{'meter-quality':10,'window-math':1})[a.fixture]:raise AssertionError('Missing independent scenario groups')
  receipt['status']='PASS'
except BaseException as error:
 receipt['error_type']=type(error).__name__
 if hasattr(error,'owned_process_cleanup'):receipt['owned_process_cleanup']=error.owned_process_cleanup
 if isinstance(error,subprocess.CalledProcessError):receipt['failed_exit']=error.returncode
finally:
 after=hashes(source);receipt['source_unchanged']=after==before
 receipt['independent_unchanged']=hashes(here)==independent
 receipt['toolchain_unchanged']=all(hashlib.sha256(path.read_bytes()).hexdigest()==manifest['toolchain'][name]['sha256']for name,path in tools.items())
 receipt['input_manifest_unchanged']=hashlib.sha256(a.input_manifest.read_bytes()).hexdigest()==a.input_manifest_sha256
 if not all(receipt[name]for name in ('source_unchanged','independent_unchanged','toolchain_unchanged','input_manifest_unchanged')):receipt['status']='FAIL'
 receipt['ended_at']=datetime.datetime.now(datetime.timezone.utc).isoformat();receipt['commands']=commands
 (out/'SUMMARY.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({k:v for k,v in receipt.items()if k not in ['source_sha256','independent_sha256','commands']}))
raise SystemExit(receipt['status']not in ['PASS','COMPILED_NOT_RUN'])
