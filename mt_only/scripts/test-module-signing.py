#!/usr/bin/env python3
"""Host module signing/runtime/channel-AES tests; no real PSA or hardware.

Optional --dependency-root is another source repository containing byte-identical
bundled Monocypher/nanopb/generated headers. Candidate policy/codec and production
sources always come from this runner's repository, never the dependency copy.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,stat,sys,time
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host
sys.path.insert(0,str(ROOT/'mt_only/tests/airtime_quality'))
from bounded_process import run_group,install_termination_handler

def require(ok,message):
 if not ok:raise RuntimeError(message)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def dependency_paths(root):
 mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source'
 return sorted(set([*mono.glob('*.c'),*mono.glob('*.h'),*proto.glob('*.pb.h'),*proto.glob('nanopb/*.c'),*proto.glob('nanopb/*.h')]))
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',required=True,type=Path)
 p.add_argument('--dependency-root',type=Path,default=ROOT,help='Repository root with identical bundled zephcore dependencies (default: this source)')
 p.add_argument('--stock',type=Path,help='Use the pinned 2.8.2 firmware Git object for a stock policy differential')
 p.add_argument('--short-enums',action='store_true')
 p.add_argument('--legacy-signed-fixture',action='store_true',help='Also-supported legacy signed-public fixture; catalog retains its existing runner')
 a=p.parse_args();require(__debug__,'Optimized Python qualification refused')
 app=ROOT/'mt_only';vendor=ROOT/'zephcore/adapters/bench/vendor';deps=a.dependency_root.resolve();out=a.output.resolve()
 require(not out.is_relative_to(ROOT)and not out.is_relative_to(deps),'Output must be outside source and dependency trees')
 canonical=dependency_paths(ROOT);external=dependency_paths(deps)
 require(canonical and {x.relative_to(ROOT)for x in canonical}=={x.relative_to(deps)for x in external},'Bundled dependency membership mismatch')
 for original in canonical:
  candidate=deps/original.relative_to(ROOT)
  require(not original.is_symlink()and not candidate.is_symlink()and digest(original)==digest(candidate),'Bundled dependency body mismatch: '+str(original.relative_to(ROOT)))
 mono=deps/'zephcore/lib/monocypher';proto=deps/'zephcore/bench/upstream-differential/source'
 mandatory=[mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in('pb_common.c','pb_encode.c','pb_decode.c','pb.h')),proto/'mesh.pb.h',proto/'portnums.pb.h',vendor/'MtModulePorts.h']
 require(all(x.is_file()for x in mandatory),'Required bundled dependency missing')
 def input_paths():
  return sorted(set([*dependency_paths(ROOT),*dependency_paths(deps),*app.glob('experimental/pki/*.cpp'),*app.glob('experimental/pki/*.h'),*app.glob('src/**/*.h'),*app.glob('src/**/*.hpp'),*app.glob('src/pki/*.cpp'),*vendor.glob('*.h'),vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',app/'src/Protobuf.cpp',app/'src/modules/BeaconExcursion.cpp',app/'src/modules/BeaconTargetRuntime.cpp',app/'tests/beacon/stubs/zephyr/kernel.h',app/'tests/software_module_signing_cases.cpp',app/'tests/signed_public_application_cases.cpp',app/'tests/endpoint_fixture.h',app/'tests/known_channel_fixture.h',app/'tests/airtime_quality/bounded_process.py',ROOT/'host_test_toolchain.py',Path(__file__).resolve()]))
 def snapshot():
  rows={}
  for x in input_paths():
   require(x.is_file()and not x.is_symlink(),'Input is not a regular source file: '+str(x))
   key=x.relative_to(ROOT).as_posix()if x.is_relative_to(ROOT)else'dependency-root/'+x.relative_to(deps).as_posix()
   rows[key]={'bytes':x.stat().st_size,'mode':stat.S_IMODE(x.stat().st_mode),'sha256':digest(x)}
  return rows
 cc=host.cc();cxx=host.cxx()
 require(cc and cxx and not any(v.startswith('-DNDEBUG')for v in cc+cxx),'Active C++ fixture assertions required')
 before=snapshot();require(len(before)>=187,'Incomplete signing qualification input closure')
 out.mkdir(parents=True,exist_ok=False)
 (out/'INPUTS.json').write_text(json.dumps(before,indent=2,sort_keys=True)+'\n')
 commands=[];processes=[];status='FAIL';error=None;started=time.monotonic();after={};changed=[]
 install_termination_handler()
 def run(argv,name):
  commands.append(list(map(str,argv)));record={'name':name,'argv':commands[-1]};processes.append(record)
  try:
   timeout=240 if name.endswith('.build.log') or name=='compile.log' else 60
   with(out/name).open('x')as log:run_group(commands[-1],timeout=timeout,stdout=log,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},cwd=out)
   record.update(status='PASS',leader_reaped=True,group_gone=True)
  except BaseException as e:record.update(status='FAIL',cleanup=getattr(e,'owned_process_cleanup',{}));raise
 try:
  (out/'meshtastic').mkdir()
  for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
  flags=['-fsanitize=address,undefined','-g','-O1','-fno-omit-frame-pointer',*['-I'+str(x)for x in(app/'src',app/'src/pki',app/'experimental/pki',app/'tests',app/'tests/beacon/stubs',vendor,mono,out,proto/'nanopb')],*host.crypto_compile_flags(),*(['-fshort-enums']if a.short_enums else[])]
  if sys.platform=='darwin':flags.append('-DEKEYREJECTED=129')
  if a.stock:
   sys.path.insert(0,str(app/'tests/stock_signature_282'))
   from oracle import generate
   generate(a.stock.resolve(),out/'stock-signing.inc')
   flags.append('-DMT_TEST_STOCK_SIGNING_282=1')
  objects=[]
  for source in[mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in('pb_common.c','pb_encode.c','pb_decode.c'))]:
   obj=out/(source.stem+'.o');run([*cc,'-std=c11',*flags,'-c',source,'-o',obj],source.stem+'.build.log');objects.append(obj)
  test=app/'tests'/('signed_public_application_cases.cpp'if a.legacy_signed_fixture else'software_module_signing_cases.cpp')
  cpp=[]if a.legacy_signed_fixture else[app/'src/pki'/f'{n}.cpp'for n in('PkiSigner','PkiRuntime','PkiSession','PkiIdentity','PkiLedger','PkiStorage','PkiAppendHistory')]+[app/'experimental/pki/PkiCrypto.cpp',app/'experimental/pki/PkiAck.cpp',app/'src/modules/BeaconExcursion.cpp',app/'src/modules/BeaconTargetRuntime.cpp']
  run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,test,app/'src/Protobuf.cpp',*([app/'src/pki/verification/KeyVerification.cpp',app/'src/pki/verification/KeyVerificationService.cpp'] if not a.legacy_signed_fixture else []),vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*cpp,*objects,*host.crypto_link_flags(),'-o',out/'tests'],'compile.log')
  run([out/'tests'],'tests.log')
  expected=9 if a.legacy_signed_fixture else (10 if a.stock else 9)
  require(sum(x.startswith('PASS ')for x in(out/'tests.log').read_text().splitlines())==expected,'Expected complete fixture group count')
  # Standalone raw codec headers must not gain a generated-schema dependency.
  unit=out/'standalone.cpp';unit.write_text('#include "MtCodec.h"\nstatic_assert(bench::moduleBroadcastPort(66));\nint main(){return 0;}\n')
  run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*(['-fshort-enums']if a.short_enums else[]),'-fsyntax-only','-I'+str(vendor),unit],'standalone.build.log')
  status='PASS'
 except BaseException as e:error=type(e).__name__+': '+str(e)
 finally:
  try:
   after=snapshot();changed=sorted(n for n in before.keys()|after.keys()if before.get(n)!=after.get(n))
  except BaseException as e:error=(error+'; 'if error else'')+'input closeout: '+type(e).__name__+': '+str(e);status='FAIL'
  if changed:status='STALE'
  text=(out/'tests.log').read_text()if(out/'tests.log').exists()else''
  receipt={'schema':'zt-module-signing-host-tests/v1','status':status,'error':error,'source_changed':changed,'input_count':len(before),'input_pins':before,'input_pins_after':after,'source_sha256':{n:v['sha256']for n,v in before.items()},'source_sha256_after':{n:v['sha256']for n,v in after.items()},'source_root':str(ROOT),'dependency_root':str(deps),'dependency_copy_identical_to_source':True,'short_enums':a.short_enums,'legacy_signed_fixture':a.legacy_signed_fixture,'groups':sum(x.startswith('PASS ')for x in text.splitlines()),'ASAN_UBSAN':status=='PASS','PSA':False,'private_CCM_backend':'deliberately unusable NoPrivateCrypto stub; focused module cases assert zero calls','actual_signer_runtime_ledger_channel_AES':not a.legacy_signed_fixture,'host_channel_AES':'OpenSSL'if sys.platform.startswith('linux')else'CommonCrypto','hardware':False,'RF':False,'commands':commands,'owned_processes':processes,'elapsed':time.monotonic()-started}
  (out/'RESULT.json').write_text(json.dumps(receipt,indent=2,sort_keys=True)+'\n');print(json.dumps({k:receipt[k]for k in('status','error','source_changed','input_count','groups','PSA','elapsed')}))
 return 0 if status=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
