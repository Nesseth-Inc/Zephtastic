#!/usr/bin/env python3
"""Actual Service/Companion/private runtime with real PSA and memory-only journals."""
import argparse,hashlib,json,os,platform,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--startup',action='store_true');p.add_argument('--short-enums',action='store_true');p.add_argument('--cases',type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--psa-root',required=True,type=Path);p.add_argument('--psa-library',required=True,type=Path);a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source';psa=a.psa_root.resolve()
pin=next(x['commit'] for x in json.loads((app/'dependency-lock.json').read_text()) if x['path']=='modules/crypto/tf-psa-crypto')
assert subprocess.check_output(['git','-C',str(psa),'rev-parse','HEAD'],text=True).strip()==pin
subprocess.run(['git','-C',str(psa),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
(out/'results.json').unlink(missing_ok=True)
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-O1',*[f'-I{x}' for x in [app/'tests',app/'src',app/'src/pki',app/'experimental/pki',vendor,mono,out,proto/'nanopb',psa/'include',psa/'drivers/builtin/include',psa/'dispatch/include']],f'-DTF_PSA_CRYPTO_CONFIG_FILE="{app / "tests/pki/psa-host-config.h"}"']
if a.cases and a.cases.name=='expanded_contact_runtime.cpp':
 flags+=['-DCONFIG_MT_XIAO_EXPANDED_CONTACTS=1']
 # Compile the entire production owner-loop body using the fixture's board
 # dependencies. No production copy, test preprocessor gate, or Zephyr headers.
 board_main=(app/'src/main.cpp').read_text()
 begin='static void pollVerifiedPeerCandidate() {'
 end='\n}\n#endif // CONFIG_MT_PKI_TARGET_RUNTIME'
 assert board_main.count(begin)==1 and board_main.count(end)==1
 body=board_main.split(begin,1)[1].split(end,1)[0]
 (out/'native-verified-candidate.inc').write_text(body+'\n')
if a.cases and a.cases.parent.name=='stock_paxcounter':
 flags+=['-DCONFIG_MT_PAXCOUNTER=1','-DCONFIG_MT_PAXCOUNTER_WIFI_ESP32=1']
extra_cpp=[]
if a.cases and a.cases.name=='imported_channel_origins.cpp':
 flags+=['-DCONFIG_MT_STOCK_IDENTITY_IMPORT=1','-I'+str(app/'tests/beacon/stubs')]
 extra_cpp.extend(app/'src/modules'/name for name in ('BeaconTargetRuntime.cpp','BeaconExcursion.cpp'))
if a.short_enums:flags+=['-fshort-enums']
if a.cases and a.cases.parent.name=='stock_optional':
 flags+=['-DCONFIG_MT_REPLY_BOT=1','-DCONFIG_MT_REPLY_EXAMPLE=1']
if a.cases and a.cases.name=='primary_channel_runtime_cases.cpp':flags+=['-DCONFIG_MT_GPIO_MODULES=1']
if a.cases and a.cases.name=='pax_query_cases.cpp':
 flags+=['-DCONFIG_MT_PAXCOUNTER=1','-DCONFIG_ZEPHCORE_AUDIO_CODEC2=1']
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));host_link=[]
if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
if platform.system()=='Darwin':flags+=['-DEKEYREJECTED=129']
sources=sorted(set([*app.glob('src/*.h'),*app.glob('src/*.hpp'),*app.glob('src/pki/*.h'),*app.glob('src/pki/*.hpp'),*app.glob('src/pki/*.cpp'),
 *app.glob('experimental/pki/Pki*.h'),*app.glob('experimental/pki/Pki*.cpp'),
 *vendor.glob('*.h'),vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',app/'src/Protobuf.cpp',
 app/'tests/pki_runtime_tests.cpp',app/'tests/pki_startup_tests.cpp',app/'tests/pki_runtime_fixture.h',Path(__file__).resolve(),
 app/'tests/pki/psa-host-config.h']))
if a.cases:sources.append(a.cases.resolve())
sources.extend(extra_cpp)
if a.cases and a.cases.name=='expanded_contact_runtime.cpp':sources.append(app/'src/main.cpp')
if a.cases and a.cases.name=='device_reset.cpp':
 sources.extend(sorted((app/'src/stock/admin').glob('*.h')))
 sources.extend(sorted((app/'src/pki/provisioning').glob('*.h')))
if a.cases and a.cases.parent.name=='stock_optional':sources.extend(sorted((app/'src/stock/optional').glob('*.h')))
if a.cases and a.cases.name=='pax_query_cases.cpp':
 sources.extend(app/'src'/name for name in ['modules/PaxQuery.h','modules/peripherals/PaxcounterCore.h','modules/audio/AudioWire.h','stock/peripherals/PaxcounterModule.h','stock/peripherals/AudioModule.h'])
def source_hashes():return {str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sources}
before=source_hashes()
commands=[];objects=[]
def run(cmd):
 commands.append([str(x) for x in cmd]);subprocess.run(cmd,check=True)
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');run([*cc,'-std=c11',*flags,'-c',src,'-o',obj]);objects.append(obj)
cpp=['PkiRuntime','PkiSession','PkiIdentity','PkiLedger','PsaPkiBackend','PkiSigner']
if a.startup or a.cases:cpp+=['PkiStorage','PkiAppendHistory']
if a.cases:cpp+=['PkiProvisioning']
test=a.cases.resolve()if a.cases else app/'tests'/('pki_startup_tests.cpp' if a.startup else 'pki_runtime_tests.cpp')
exe=out/'runtime-tests'
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,test,app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*extra_cpp,*[app/f'src/pki/{n}.cpp' for n in cpp],app/'experimental/pki/PkiCrypto.cpp',app/'experimental/pki/PkiAck.cpp',*objects,a.psa_library.resolve(),*host_link,'-o',exe])
r=subprocess.run([exe],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/'tests.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
assert source_hashes()==before, 'source changed during validation'
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps({'status':'PASS','groups':sum(x.startswith('PASS ') for x in r.stdout.splitlines()),'ASAN_UBSAN':True,'short_enums':a.short_enums,'backend':'pinned real PSA','psa_revision':pin,'psa_library_sha256':hashlib.sha256(a.psa_library.read_bytes()).hexdigest(),'devices':False,'credentials':'public deterministic fixtures only','source_stable':True,'source_sha256':before})+'\n')
