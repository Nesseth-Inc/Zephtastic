#!/usr/bin/env python3
"""Exercise public report pages, native NOR, and the real PhoneAPI."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here=Path(__file__).resolve().parent
app=here.parents[1];root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stock',type=Path,required=True);p.add_argument('--littlefs',type=Path,required=True)
a=p.parse_args()
donor=subprocess.check_output(['git','-C',a.stock,'show','8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c:src/mesh/NodeDB.cpp'],text=True)
oracle=donor[donor.index('template <typename Map> bool evictStalestSatellite'):donor.index('} // namespace\n\nvoid NodeDB::resetRadioConfig')]
proto=root/'zephcore/bench/upstream-differential/source';mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
hashes={'lfs.c':'e36dfc19a2e8516024d764c9713c85a2b26a80cf0f4d9cd862c4104771e170e8','lfs.h':'b1befd7288d08815accc8f9af744c55686c0b9e3ac0061c32ceee38a1b3eb96d','lfs_util.c':'f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec','lfs_util.h':'548d46aa524dc7449e16739286c1a422a52f9de727ff0be0c2ffc5593f5ca981'}
with tempfile.TemporaryDirectory(prefix='zt-paged-reports-') as temp:
    out=Path(temp);(out/'meshtastic').mkdir();(out/'oracle.inc').write_text(oracle)
    for h in proto.glob('*.pb.h'):(out/'meshtastic'/h.name).symlink_to(h)
    for file,digest in hashes.items():
        data=(a.littlefs/file).read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:raise ValueError(file)
        (out/file).write_bytes(data)
    for abi in ([],['-fshort-enums']):
        flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-DCONFIG_MT_T096_FULL_COMPANION=1','-DCONFIG_MT_PEER_DIRECTORY=1','-DCONFIG_MT_SATELLITE_HISTORY=1','-DCONFIG_MT_WARM_PEERS=1','-DCONFIG_MT_PAGED_SATELLITES=1',
            '-DLFS_NO_DEBUG','-DLFS_NO_WARN','-DLFS_NO_ERROR','-D__maybe_unused=__attribute__((unused))',
            *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else []),*host.crypto_compile_flags(),
            *['-I'+str(v) for v in (out,app/'src',app/'tests',app/'tests/stock_warm_peers/mocks',app/'src/pki',app/'experimental/pki',vendor,mono,proto/'nanopb')],*abi]
        objs=[]
        for src in [out/'lfs.c',out/'lfs_util.c',mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/s for s in ('pb_common.c','pb_decode.c','pb_encode.c')]]:
            obj=out/(src.stem+'.o');objs.append(obj);subprocess.run([*host.cc(),'-std=c11',*flags,'-c',src,'-o',obj],check=True)
        volumes=[out/s for s in ('old-t096-16','old-t096-32','old-xiao-16')]
        for config in (['-UCONFIG_MT_PAGED_SATELLITES'],[]):
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*config,here/'compat.cpp',
                *objs,*host.crypto_link_flags(),'-o',out/'compat'],check=True)
            subprocess.run([out/'compat',*volumes],check=True)
        for cap in (40,250):
            subprocess.run([*host.cxx(),'-std=c++17',*flags,'-DMAX_SATELLITE_NODES='+str(cap),
                here/'oracle.cpp','-o',out/'oracle'],check=True)
            subprocess.run([out/'oracle'],check=True)
        for case in ('numeric','store','phone','nor','protected_nor'):
            inputs=[here/(('nor' if case=='protected_nor' else case)+'.cpp')]
            if case=='protected_nor':inputs.insert(0,'-DCONFIG_MT_XIAO_LARGE_NODEDB=1')
            if case=='numeric':inputs += [app/'src/Protobuf.cpp']
            if case=='phone':inputs += [app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp']
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*inputs,*objs,*host.crypto_link_flags(),'-o',out/case],check=True)
            subprocess.run([out/case],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
