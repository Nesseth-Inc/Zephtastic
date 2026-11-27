#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the target owner against the pinned Meshtastic hook, hash and local delay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host

STOCK='d5f16191ca28b01b5024f590f04a67f25d66f1b1'
PINS={'src/mesh/Channels.cpp':'509d7b9d7c82ee64f4ec3b49629083c63705c0cf5ec52b0515052c1356ee60ff',
      'src/mesh/RadioInterface.cpp':'c35915a056695b6cd471c30f1d103b731033f7ede157fbbcb67cd82d4ef63a9f',
      'src/mesh/RadioLibInterface.cpp':'bc339bdaf13052aad65b0c664a3270a0705c757ff464e95d85d946ec62f81229',
      'src/modules/MeshBeaconModule.cpp':'c7e825667b18792af2ff8a7a58e33f399fc41834c0412d5b6d45fcfe35b7b470',
      'test/test_mesh_beacon/test_main.cpp':'badcbecf1e8c4b8e12734fc7265ce810d2b6cfab1c1ede7f0db79cc9a4f817ca'}


def method(source, signature):
    start=source.index(signature)
    return source[start:source.index('\n}',source.index('\n{',start))+2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    sources={}
    for name,digest in PINS.items():
        data=(args.stock/name).read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:
            raise ValueError(f'Unexpected stock source: {name}')
        sources[Path(name).name]=data.decode()
    units=[method(sources['Channels.cpp'],sig) for sig in ('uint8_t xorHash(', 'int16_t Channels::hashFor(')]
    units.extend(method(sources['RadioInterface.cpp'],sig) for sig in (
        'uint32_t RadioInterface::getTxDelayMsec(', 'uint32_t RadioInterface::computeSlotTimeMsec('))
    units.append(method(sources['MeshBeaconModule.cpp'],'RadioTxHook::PreTxAction MeshBeaconTxHook::beforeTransmit('))
    radio=sources['RadioLibInterface.cpp']
    defer=radio.index('action == RadioTxHook::PRETX_DEFER')
    if not defer < radio.index('setTransmitDelay();',defer) < radio.index('isChannelActive();',defer):
        raise ValueError('Donor retune path must defer before target CAD')
    proto=ROOT/'zephcore/bench/upstream-differential/source'
    vendor=ROOT/'zephcore/adapters/bench/vendor'
    mono=ROOT/'zephcore/lib/monocypher'
    results=[]
    with tempfile.TemporaryDirectory(prefix='beacon-target-') as tmp:
        build=Path(tmp)
        (build/'meshtastic').symlink_to(proto,target_is_directory=True)
        (build/'donor.inc').write_text('\n'.join(units))
        owner=(APP/'src/main.cpp').read_text()
        start=owner.index('static void drainChannelObservations(')
        (build/'meter-owner.inc').write_text(owner[start:owner.index('\n}',start)+2])
        for label,abi in [('normal',[]),('short',['-fshort-enums'])]:
            flags=['-O1','-UNDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all',*abi,
                   *(('-DEKEYREJECTED=129',) if sys.platform=='darwin' else ()),
                   *['-I'+str(p) for p in (build,APP/'src',APP/'tests',APP/'tests/beacon/stubs',vendor,mono,proto/'nanopb')]]
            objects=[]
            for unit in [mono/'monocypher.c',mono/'monocypher-ed25519.c',
                         *[proto/'nanopb'/f'pb_{n}.c' for n in ('common','encode','decode')]]:
                obj=build/(unit.stem+'.o')
                subprocess.run([*host.cc(),'-std=c11',*flags,'-c',unit,'-o',obj],check=True)
                objects.append(obj)
            exe=build/'target'
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*host.crypto_compile_flags(),HERE/'cases.cpp',
                            APP/'src/modules/BeaconTargetRuntime.cpp',APP/'src/modules/BeaconExcursion.cpp',
                            APP/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objects,
                            *host.crypto_link_flags(),'-o',exe],check=True)
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
            row=json.loads(subprocess.check_output([exe],env=env,text=True))
            if row!={'target_rechecks':2,'fifo_frames':4,'runtime_scenarios':15,'refilled_slots':24}:
                raise ValueError(f'Incomplete beacon runtime run: {row}')
            results.append({'enum':label,**row})
    report={'status':'PASS','hardware':False,'stock_revision':STOCK,'source_sha256':PINS,'runs':results}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(results))

if __name__=='__main__':
    main()
