#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile production identity import and exercise isolated synthetic storage."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

tests=Path(__file__).resolve().parent
root=tests.parents[2]
sys.path.insert(0,str(root))
from host_test_toolchain import cc, cxx, crypto_compile_flags, crypto_link_flags

app=root/'mt_only'
vendor=root/'zephcore/adapters/bench/vendor'
mono=root/'zephcore/lib/monocypher'
pki=app/'src/pki'
flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DCONFIG_MT_STOCK_IDENTITY_IMPORT=1',
       *crypto_compile_flags(),*[f'-I{d}' for d in (app/'src',pki,app/'experimental/pki',vendor,mono)]]
sources=[*(pki/(name+'.cpp') for name in ('PkiIdentity','PkiProvisioning','PkiLedger','PkiAppendHistory','PkiStorage')),
         vendor/'MtSignature.cpp',vendor/'MtCodec.cpp',tests/'storage.cpp']
with tempfile.TemporaryDirectory() as path:
    out=Path(path)
    proto=root/'zephcore/bench/upstream-differential/source'
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
    flags.extend([f'-I{out}',f'-I{proto/"nanopb"}',f'-I{app/"tests"}'])
    if sys.platform=='darwin':flags.append('-DEKEYREJECTED=129')
    for label,extra in [('normal',[]),('short',['-fshort-enums'])]:
        objs=[]
        for c in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/f for f in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
            obj=out/(c.stem+'.o');subprocess.run([*cc(),'-std=c11',*flags,*extra,'-c',c,'-o',obj],check=True);objs.append(obj)
        exe=out/label
        subprocess.run([*cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,*extra,*sources,*objs,*crypto_link_flags(),'-o',exe],check=True)
        subprocess.run([exe],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
        companion=out/('companion-'+label)
        subprocess.run([*cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,*extra,'-DCONFIG_MT_RF_ADMIN_GET=1',
                        tests/'companion.cpp',app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objs,
                        *crypto_link_flags(),'-o',companion],check=True)
        subprocess.run([companion],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})

        recovery=out/('recovery-'+label)
        subprocess.run([*cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,*extra,
                        *sources[:-1],tests/'recovery.cpp',app/'src/Protobuf.cpp',*objs,*crypto_link_flags(),'-o',recovery],check=True)
        subprocess.run([recovery],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
