#!/usr/bin/env python3
"""Fault and lifecycle tests for the dormant PKI ledger; synthetic host only."""
import argparse,hashlib,json,os,platform,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--cases',type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1];root=app.parent;vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
cases=a.cases.resolve()if a.cases else app/'tests/pki_ledger_tests.cpp'
if not cases.is_relative_to(root):p.error('Case source must belong to the exported source tree')
sources=[app/'src/pki/PkiLedger.cpp',app/'src/pki/PkiAppendHistory.cpp',app/'src/pki/PkiIdentity.cpp',vendor/'MtSignature.cpp',vendor/'MtCodec.cpp',cases]
tracked=sources+[app/'src/pki/PkiAppendHistory.h',app/'src/pki/AppendStore.h',app/'tests/pki_ledger_tests.cpp',app/'src/pki/PkiLedger.h',app/'src/pki/PkiReplayAuthority.h',app/'src/pki/PkiSession.h',app/'src/pki/PkiKeyProvider.h',
                 mono/'monocypher.c',mono/'monocypher-ed25519.c',Path(__file__).resolve()]
def hashes():return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in tracked}
before=hashes()
flags=['-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
       '-ffunction-sections','-fdata-sections','-I'+str(app/'src'),'-I'+str(app/'experimental/pki'),'-I'+str(vendor),'-I'+str(mono)]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
link=[]
if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL'];link+=['-lcrypto']
commands=[]
def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
objects=[]
for name in ['monocypher.c','monocypher-ed25519.c']:
    obj=out/(name+'.o');objects.append(obj);run([*cc,'-std=c99',*flags,'-c',str(mono/name),'-o',str(obj)])
run([*cxx,'-std=c++17',*flags,'-fno-exceptions','-fno-rtti','-c',str(sources[0]),'-o',str(out/'ledger-no-exceptions.o')])
run([*cxx,'-std=c++17',*flags,*map(str,sources),*map(str,objects),*link,'-o',str(out/'ledger-tests')])
r=subprocess.run([str(out/'ledger-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();summary=json.loads(r.stdout.splitlines()[-1])
after=hashes()
if after!=before:raise RuntimeError('Source changed during tests; no success receipt produced')
summary.update(ASAN_UBSAN=True,no_exceptions_rtti_compile=True,source_stable=True,hardware=False,persistent_credentials_created=False,
               entropy='injected synthetic test source only; no target adapter',commands=commands,
               source_sha256=after)
(out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
