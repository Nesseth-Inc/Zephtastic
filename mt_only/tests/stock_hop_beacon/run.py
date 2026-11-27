#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run HopScaling and MeshBeacon against the locked Meshtastic 2.8.2 sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

STOCK = 'd5f16191ca28b01b5024f590f04a67f25d66f1b1'
PINS = {
    'mesh/MeshModule.cpp': 'ff8c9c4ba2bff5d122346451e898fe109a9b15e04189b642b366730b335398f7',
    'mesh/MeshModule.h': '82c7362f256da67ccf84a9e0e30564ac223d5d1416b9476cd594e7fafd22a9b4',
    'modules/HopScalingModule.cpp': '1998c4c163862099894b6f6f73871ed9d843cd9ab8566baa436a1fcdf4fb8aaa',
    'modules/HopScalingModule.h': '3e27aa82ca4cd7305b738a56809e65fdf9785d72dbc7393b7fb6f58c543e9b91',
    'modules/MeshBeaconModule.cpp': 'c7e825667b18792af2ff8a7a58e33f399fc41834c0412d5b6d45fcfe35b7b470',
    'modules/MeshBeaconModule.h': '8f32ade6824e5754ef245fbafa49baf2f8b8061776cd9f402487a1d18701a4fd',
    'mesh/NodeDB.cpp': '006921660b5b4b7e8ee32fecc4dcb84cde3e31d0c0853c335e0c43b02f31d3e1',
    'mesh/MeshTypes.h': '0d0e585f3c717250153d1109d7798999af393a2a017f21fe3c6b200d10f62675',
    'mesh/Router.cpp': '40a14acf1e34abe5b8ab2c37269c7600f505488d01c73c2e5b9c23e5d0c3346c',
    'mesh/Default.cpp': 'a0a46f1b5a6173fc0365f4cb50353fb52ae6748ee2e036ddec4a7900918f9443',
    'mesh/Default.h': '0a468de454cb7da41536b90715cde20d506b2430d3577310a32347b67fed0116',
    'mesh/Throttle.cpp': '1a14f30656f527e8734c75656ba081e5c19dc314c999bb4d5f3fe7d2a68eb063',
}


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', source.index('\n{', start)) + 2]


def prepare(build, sources):
    for name in ('HopScalingModule.cpp', 'HopScalingModule.h'):
        (build / name).write_text(sources['modules/' + name])
    for stub in ('SafeFile.h', 'meshUtils.h', 'FSCommon.h', 'NodeDB.h', 'SPILock.h', 'airtime.h',
                 'concurrency/LockGuard.h', 'mesh-pb-constants.h', 'mesh/mesh-pb-constants.h', 'configuration.h'):
        f = build / stub
        f.parent.mkdir(exist_ok=True)
        f.touch()
    (build / 'mesh/Default.h').write_text('\n'.join(
        line for line in sources['mesh/Default.h'].splitlines() if line.startswith('#define default_hop_scaling_')))
    (build / 'MeshTypes.h').write_text(
        '#pragma once\n#include <meshtastic/mesh.pb.h>\n#include <meshtastic/config.pb.h>\n'
        '#define HOP_MAX 7\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n'
        'struct StockConfig {meshtastic_Config_DeviceConfig device{};}; extern StockConfig config;\n')
    (build / 'concurrency/OSThread.h').write_text(
        '#pragma once\nnamespace concurrency { struct OSThread { OSThread(const char*){} '
        'inline static unsigned startup=0; void setIntervalFromNow(unsigned x){startup=x;} virtual int32_t runOnce()=0; }; }\n')
    types = sources['mesh/MeshTypes.h']
    constants = types[types.index('#define NODENUM_BROADCAST '):types.index('#define ERRNO_OK')]
    (build / 'node-address.inc').write_text(constants +
        method(sources['mesh/NodeDB.cpp'], 'bool isFromUs(') + '\n' +
        method(sources['mesh/NodeDB.cpp'], 'bool isBroadcast('))
    router = sources['mesh/Router.cpp']
    start = router.index('    if (isFromUs(p) && isBroadcast(p->to) && hopScalingModule')
    (build / 'router.inc').write_text(router[start:router.index('\n#endif', start)])
    header = sources['mesh/MeshModule.h']
    delays = [line for line in header.splitlines() if line.startswith('#define MESHMODULE_')
              and ('MIN_BROADCAST_DELAY_MS' in line or 'BROADCAST_SPACING_MS' in line)]
    (build / 'periodic-start.inc').write_text('\n'.join(delays) + '\n' +
        method(sources['mesh/MeshModule.cpp'], 'int32_t MeshModule::setStartDelay('))
    beacon = sources['modules/MeshBeaconModule.cpp']
    segments = [method(sources['mesh/Default.cpp'], 'uint32_t Default::' + name + '(')
                for name in ('getConfiguredOrDefault', 'getConfiguredOrMinimumValue')]
    segments.append(method(sources['mesh/Throttle.cpp'], 'bool Throttle::isWithinTimespanMs('))
    segments += [line for line in sources['mesh/Default.h'].splitlines()
                 if line.startswith('#define default_mesh_beacon_min_broadcast_interval_secs')]
    segments += [line for line in sources['modules/MeshBeaconModule.h'].splitlines()
                 if line.startswith('#define MESH_BEACON_FLAG_')]
    segments += [method(beacon, signature) for signature in (
        'static uint32_t beaconIntervalMs()', 'void MeshBeaconModule::beaconChannelName(',
        'uint32_t MeshBeaconModule::offerFrequencySlot(', 'bool MeshBeaconModule::offerSlotUsable(',
        'void MeshBeaconModule::fillOffer(', 'void MeshBeaconBroadcastModule::rebuildCache()',
        'void MeshBeaconBroadcastModule::sendBeacon()', 'int32_t MeshBeaconBroadcastModule::runOnce()')]
    (build / 'beacon.inc').write_text('\n'.join(segments))
    fixture = (ROOT / 'mt_only/tests/beacon_controller_tests.cpp').read_text()
    (build / 'zt-fixture.inc').write_text(fixture[:fixture.index('static void frequencySlots()')])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stock', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    sources = {}
    for name, digest in PINS.items():
        data = (args.stock / 'src' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{name} does not match pinned Meshtastic {STOCK}')
        sources[name] = data.decode()
    proto = ROOT / 'zephcore/bench/upstream-differential/source'
    mono = ROOT / 'zephcore/lib/monocypher'
    vendor = ROOT / 'zephcore/adapters/bench/vendor'
    app = ROOT / 'mt_only'
    runs = []
    totals = {'hop_scaling': 0, 'mesh_beacon': 0}
    with tempfile.TemporaryDirectory(prefix='hop-beacon-') as tmp:
        build = Path(tmp)
        (build / 'meshtastic').symlink_to(proto, target_is_directory=True)
        prepare(build, sources)
        for label, abi in [('normal', []), ('short', ['-fshort-enums'])]:
            flags = ['-O1', '-UNDEBUG', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', *abi]
            if sys.platform == 'darwin':
                flags.append('-DEKEYREJECTED=129')
            inc = ['-I' + str(x) for x in (build, app/'src', app/'tests', app/'tests/beacon/stubs',
                                             mono, vendor, proto/'nanopb')]
            cxx = [*host.cxx(), '-std=c++17', *flags, *inc]
            env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
            objects = []
            for unit in [mono/'monocypher.c', mono/'monocypher-ed25519.c',
                         *[proto/'nanopb'/f'pb_{n}.c' for n in ('common', 'encode', 'decode')]]:
                obj = build/(unit.stem + '.o')
                subprocess.run([*host.cc(), '-std=c11', *flags, *inc, '-c', unit, '-o', obj], check=True)
                objects.append(obj)
            for exe, extra in [('hops', ['-DHAS_VARIABLE_HOPS=1', '-DPIO_UNIT_TESTING', build/'HopScalingModule.cpp']),
                               ('beacon', [app/'src/modules/BeaconTargetRuntime.cpp', app/'src/modules/BeaconExcursion.cpp'])]:
                subprocess.run([*cxx, *host.crypto_compile_flags(), HERE/(exe+'.cpp'), *extra, app/'src/Protobuf.cpp',
                                vendor/'MtCodec.cpp', vendor/'MtSignature.cpp', *objects, *host.crypto_link_flags(),
                                '-o', build/exe], check=True)
            for module, exe in [('hop_scaling', 'hops'), ('mesh_beacon', 'beacon')]:
                row = json.loads(subprocess.check_output([build/exe], env=env, text=True))
                count = row['comparisons']
                if count <= 0:
                    raise ValueError(f'Empty {module} run')
                totals[module] += count
                runs.append({'module': module, 'enum': label, **row})
    result = dict(status='PASS', stock_revision=STOCK, comparisons=totals, runs=runs,
                  input_sha256=PINS, asan_ubsan=True, hardware=False)
    args.output.parent.mkdir(exist_ok=True, parents=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(totals))


if __name__ == '__main__':
    main()
