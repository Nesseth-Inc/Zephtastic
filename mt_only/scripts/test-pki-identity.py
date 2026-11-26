#!/usr/bin/env python3
"""Host-only identity lifecycle tests, real Monocypher and synthetic memory NVS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1]
root=app.parent
sys.path.insert(0,str(root));import host_test_toolchain as host
vendor=root/'zephcore/adapters/bench/vendor'
mono=root/'zephcore/lib/monocypher'
flags=['-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
       '-ffunction-sections','-fdata-sections','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono)]
commands=[]
def run(command):
    commands.append(command)
    subprocess.run(command,check=True)
objects=[]
for name in ['monocypher.c','monocypher-ed25519.c']:
    obj=out/(name+'.o');objects.append(obj)
    run([*host.cc(),'-std=c99',*flags,'-c',str(mono/name),'-o',str(obj)])
sources=[app/'src/pki/PkiIdentity.cpp',vendor/'MtSignature.cpp',vendor/'MtCodec.cpp',app/'tests/pki_identity_tests.cpp']
run([*host.cxx(),'-std=c++17',*flags,'-fno-exceptions','-fno-rtti','-c',str(sources[0]),'-o',str(out/'identity-no-exceptions.o')])
run([*host.cxx(),'-std=c++17',*flags,*map(str,sources),*map(str,objects),'-o',str(out/'identity-tests')])
result=subprocess.run([str(out/'identity-tests')],text=True,capture_output=True,
                      env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr,end='')
result.check_returncode()
summary=json.loads(result.stdout)
summary.update(ASAN_UBSAN=True,no_exceptions_rtti_compile=True,hardware=False,persistent_credentials_created=False,
               backend='Monocypher 4.0.3 X25519; synthetic in-memory SettingsBackend',commands=commands,
               source_sha256={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest()
                 for f in sources+[app/'src/pki/PkiIdentity.h',app/'src/pki/PkiKeyProvider.h',mono/'monocypher.c',mono/'monocypher-ed25519.c']})
(out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
