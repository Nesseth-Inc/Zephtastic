#!/usr/bin/env python3
"""Exercise cold PhoneAPI replay against the actual pinned LittleFS on synthetic NOR."""
import argparse, hashlib, os, subprocess, sys, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--littlefs',type=Path,required=True)
args=p.parse_args()
here=Path(__file__).resolve().parent
app=here.parents[1]; root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
proto=root/'zephcore/bench/upstream-differential/source'
mono=root/'zephcore/lib/monocypher'
vendor=root/'zephcore/adapters/bench/vendor'
pin='e9a8638fc228e43a3156ed3ac602229af3e3c1d5'
with tempfile.TemporaryDirectory(prefix='zt-satellite-history-') as t:
    out=Path(t); (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):(out/'meshtastic'/h.name).symlink_to(h)
    # Release dependency exports may omit .git. Verify their bytes against the
    # same four blobs selected by a checkout of the locked Zephyr west revision.
    hashes={
        'lfs.c':'e36dfc19a2e8516024d764c9713c85a2b26a80cf0f4d9cd862c4104771e170e8',
        'lfs.h':'b1befd7288d08815accc8f9af744c55686c0b9e3ac0061c32ceee38a1b3eb96d',
        'lfs_util.c':'f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec',
        'lfs_util.h':'548d46aa524dc7449e16739286c1a422a52f9de727ff0be0c2ffc5593f5ca981',
    }
    for name,expected in hashes.items():
        source=(subprocess.check_output(['git','-C',args.littlefs,'show',pin+':'+name])
                if (args.littlefs/'.git').exists() else (args.littlefs/name).read_bytes())
        if hashlib.sha256(source).hexdigest()!=expected:raise ValueError(f'{name} differs from {pin}')
        (out/name).write_bytes(source)
    mocks=out/'floor-headers/zephyr'
    for sub in ('drivers','storage'):(mocks/sub).mkdir(parents=True)
    (mocks/'devicetree.h').write_text('#define DT_NODE_EXISTS(n) (n)\n#define DT_NODELABEL(n) n\n#define mt_sat_fs TEST_PRIVATE_VOLUME\n#define mt_reset_floor 0\n')
    (mocks/'drivers/flash.h').write_text('#pragma once\n#include <stddef.h>\nstruct device{}; const device *flashDevice();bool device_is_ready(const device *);int flash_read(const device *,unsigned,void *,size_t);\n')
    (mocks/'storage/flash_map.h').write_text('#define FIXED_PARTITION_DEVICE(n) flashDevice()\n#define FIXED_PARTITION_OFFSET(n) 0xd4000\n#define FIXED_PARTITION_SIZE(n) (TEST_PRIVATE_VOLUME?0x1f000:0x20000)\n')
    for private in (0,1):
        subprocess.run([*host.cxx(),'-std=c++17','-I'+str(mocks.parent),'-I'+str(app/'src'),
                        '-DCONFIG_BOARD_HELTEC_T096=1','-DTEST_PRIVATE_VOLUME='+str(private),
                        here/'floor.cpp','-o',out/'floor'],check=True)
        subprocess.run([out/'floor'],check=True)
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-fsanitize=address,undefined','-O1','-g','-DCONFIG_MT_SATELLITE_HISTORY=1','-DCONFIG_MT_WARM_PEERS=1','-DCONFIG_MT_PEER_DIRECTORY=1',
               '-D__maybe_unused=__attribute__((unused))','-DLFS_NO_DEBUG','-DLFS_NO_WARN','-DLFS_NO_ERROR',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else []),*host.crypto_compile_flags(),
               *['-I'+str(path) for path in (out,app/'src',app/'tests',app/'src/pki',app/'experimental/pki',vendor,mono,proto/'nanopb')]]
        objs=[]
        for src in (out/'lfs.c',out/'lfs_util.c',mono/'monocypher.c',mono/'monocypher-ed25519.c',
                    *[proto/'nanopb'/c for c in ('pb_common.c','pb_encode.c','pb_decode.c')]):
            obj=out/(src.stem+'.o');objs.append(obj)
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',src,'-o',obj],check=True)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,here/'cases.cpp',app/'src/Protobuf.cpp',
                        vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objs,*host.crypto_link_flags(),'-o',out/'test'],check=True)
        subprocess.run([out/'test'],env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'},check=True)
