#!/usr/bin/env python3
"""Build the pinned donor census/telemetry with both target enum ABIs."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
ROOT = APP.parent
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
args = parser.parse_args()
pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
def function(path, start):
    data = subprocess.check_output(['git', '-C', args.stock, 'show', pin + ':' + path], text=True)
    begin = data.index(start)
    return data[begin:data.index('\n}', begin) + 2]

db = 'src/mesh/NodeDB.cpp'
oracle = '\n'.join((function(db, 'uint32_t sinceLastSeen(const'),
                    function(db, '#define NUM_ONLINE_SECS'),
                    function('src/modules/Telemetry/DeviceTelemetry.cpp',
                             'meshtastic_Telemetry DeviceTelemetryModule::getLocalStatsTelemetry()')))
proto = ROOT / 'zephcore/bench/upstream-differential/source'
vendor = ROOT / 'zephcore/adapters/bench/vendor'
mono = ROOT / 'zephcore/lib/monocypher'
with tempfile.TemporaryDirectory(prefix='zt-localstats-') as tmp:
    out = Path(tmp)
    (out / 'oracle.inc').write_text(oracle)
    radio='src/mesh/RadioLibInterface.cpp'
    radio_methods=('void RadioLibInterface::updateNoiseFloor()', 'uint8_t RadioLibInterface::getNoiseFloorSampleCountInternal()',
                   'int32_t RadioLibInterface::getAverageNoiseFloorInternal()', 'bool RadioLibInterface::hasNoiseFloorSamples()')
    (out/'noise.inc').write_text('\n'.join(function(radio, name) for name in radio_methods))
    (out / 'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out / 'meshtastic' / h.name).symlink_to(h)
    for abi in ([], ['-fshort-enums']):
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_MT_PEER_DIRECTORY=1', '-D__maybe_unused=__attribute__((unused))', *abi,
                 *(['-DEKEYREJECTED=129'] if sys.platform == 'darwin' else []),
                 *['-I' + str(p) for p in (out, APP/'tests', APP/'src', APP/'src/pki',
                     APP/'experimental/pki', vendor, mono, proto/'nanopb')], *host.crypto_compile_flags()]
        objects = []
        for src in [mono/'monocypher.c', mono/'monocypher-ed25519.c',
                    *[proto/'nanopb'/n for n in ('pb_common.c','pb_decode.c','pb_encode.c')]]:
            obj = out / (src.stem + '.o')
            subprocess.run([*host.cc(), '-std=c11', *flags, '-c', src, '-o', obj], check=True)
            objects.append(obj)
        subprocess.run([*host.cxx(), '-std=c++17', *flags, HERE/'cases.cpp', APP/'src/Protobuf.cpp',
                        vendor/'MtCodec.cpp', vendor/'MtSignature.cpp', *objects, *host.crypto_link_flags(),
                        '-o', out/'cases'], check=True)
        subprocess.run([out/'cases'], check=True, env={**os.environ, 'UBSAN_OPTIONS':'halt_on_error=1'})
