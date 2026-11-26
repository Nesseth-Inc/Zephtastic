#!/usr/bin/env python3
"""Independent source-bound startup probes, memory-only, real pinned PSA crypto."""
import argparse,hashlib,json,os,platform,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('source','output','psa-root','psa-library'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'RESULTS.json').unlink(missing_ok=True)
app=root/'mt_only';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source';psa=a.psa_root.resolve();lib=a.psa_library.resolve()
test=Path(__file__).resolve().parent/'m11_startup_adversarial.cpp'
pin=next(x['commit'] for x in json.loads((app/'dependency-lock.json').read_text()) if x['path']=='modules/crypto/tf-psa-crypto')
assert subprocess.check_output(['git','-C',str(psa),'rev-parse','HEAD'],text=True).strip()==pin
subprocess.run(['git','-C',str(psa),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
def sha(x):return hashlib.sha256(x.read_bytes()).hexdigest()
def state():
    paths=[x for d in (app/'src',app/'experimental/pki',mono,vendor,proto) for x in d.rglob('*') if x.is_file() and x.suffix in ('.cpp','.c','.h')]
    paths.extend([app/'tests/pki_runtime_fixture.h',app/'tests/pki_startup_tests.cpp',app/'tests/pki/psa-host-config.h',app/'dependency-lock.json'])
    return {str(x.relative_to(root)):sha(x) for x in sorted(paths)}
def revision():
    if (root/'.git').exists():return subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    manifest=root.parent/'SOURCE-MANIFEST.json'
    return json.loads(manifest.read_text())['source_revision'] if manifest.exists() else None
fixture=(app/'tests/pki_startup_tests.cpp').read_text().split('static void group(',1)[0]
assert 'struct BootFixture' in fixture and 'int main(' not in fixture
(out/'startup_review_fixture.h').write_text(fixture)
before=state();head=revision();libhash=sha(lib);testhash=sha(test);runnerhash=sha(Path(__file__))
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
includes=[app/'src',app/'tests',app/'src/pki',app/'experimental/pki',vendor,mono,out,proto/'nanopb',psa/'include',psa/'drivers/builtin/include',psa/'dispatch/include',lib.parent.parent/'include']
flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1',*[f'-I{x}' for x in includes],f'-DTF_PSA_CRYPTO_CONFIG_FILE="{app / "tests/pki/psa-host-config.h"}"']
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));host_link=[]
if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
commands=[];objects=[]
def run(cmd):
    cmd=list(map(str,cmd));commands.append(cmd);subprocess.run(cmd,check=True)
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
    obj=out/(src.stem+'.o');run([*cc,'-std=c11',*flags,'-c',src,'-o',obj]);objects.append(obj)
cpp=['PkiRuntime','PkiSession','PkiIdentity','PkiLedger','PsaPkiBackend','PkiSigner','PkiStorage','PkiAppendHistory'];exe=out/'runtime-tests'
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,test,app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*[app/f'src/pki/{n}.cpp' for n in cpp],app/'experimental/pki/PkiCrypto.cpp',app/'experimental/pki/PkiAck.cpp',*objects,lib,*host_link,'-o',exe])
r=subprocess.run([exe],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=state()==before and revision()==head and sha(lib)==libhash and sha(test)==testhash and sha(Path(__file__))==runnerhash
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n')
(out/'RESULTS.json').write_text(json.dumps({'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_commit':head,'source_stable':stable,'groups':r.stdout.splitlines(),'exit_code':r.returncode,'ASAN_UBSAN':True,'backend':'pinned real PSA','psa_revision':pin,'psa_library_sha256':libhash,'hardware':False,'credentials':'public deterministic fixtures only','fixture_boundary':'Shared author memory/radio/PSA and boot fixture declarations extracted unchanged; independent assertions and runner.','test_sha256':testhash,'runner_sha256':runnerhash},indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
