#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the Meshtastic 2.8.2 radio timing methods against deterministic host I/O."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

STOCK_COMMIT = 'd5f16191ca28b01b5024f590f04a67f25d66f1b1'
PINS = {
    'generated/meshtastic/config.pb.h': '23954f24cca6d339d6940b704873d41325bea5a0048b880023493a4bbf96ed10',
    'RadioInterface.cpp': 'c35915a056695b6cd471c30f1d103b731033f7ede157fbbcb67cd82d4ef63a9f',
    'RadioInterface.h': '4d7b33f454886c661a9f4f762142d9faaa987bf1cb29d0d5c4dcf983aa1d2ae3',
    'SX126xInterface.h': 'b9976b39494ddd39fa3e3436269ad7710874fafea837a9cd64c9358bf6cb0334',
}
METHODS = (
    'uint8_t RadioInterface::getCadSymbolCount()',
    'uint32_t RadioInterface::computeSlotTimeMsec()',
    'uint8_t RadioInterface::getCWsize(',
    'uint32_t RadioInterface::getTxDelayMsecWeightedWorst(',
    'bool RadioInterface::shouldRebroadcastEarlyLikeRouter(',
    'uint32_t RadioInterface::getTxDelayMsecWeighted(',
)


def declaration(source, prefix):
    matches = [line for line in source.splitlines() if line.strip().startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f'Expected one pinned declaration for {prefix}')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--short-enums', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = {}
    for name, digest in PINS.items():
        data = (args.stock / 'src' / 'mesh' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{name} does not match Meshtastic {STOCK_COMMIT}')
        sources[name] = data.decode()

    config = sources['generated/meshtastic/config.pb.h']
    start = config.index('typedef enum _meshtastic_Config_DeviceConfig_Role {')
    end = '} meshtastic_Config_DeviceConfig_Role;'
    (out / 'stock-device-role.inc').write_text(config[start:config.index(end,start)+len(end)])
    # Use the same whole-function extractor as the wider routing differential.
    module = importlib.util.spec_from_file_location(
        'routing_differential', ROOT / 'zephcore/bench/run-routing-differential.py')
    differential = importlib.util.module_from_spec(module)
    module.loader.exec_module(differential)
    (out / 'stock-radio.inc').write_text('\n'.join(
        differential.extract(sources['RadioInterface.cpp'], method)[0] for method in METHODS))
    (out / 'stock-cw.inc').write_text('\n'.join(
        declaration(sources['RadioInterface.h'], 'static constexpr uint8_t ' + name + ' =')
        for name in ('CWmin', 'CWmax')))
    (out / 'stock-sx126x-cad.inc').write_text(
        declaration(sources['SX126xInterface.h'], 'uint8_t getCadSymbolCountSubGhz() const override'))

    exe = out / 'compare-timing'
    cmd = [*host.cxx(), *(['-fshort-enums'] if args.short_enums else []), '-std=c++17', '-UNDEBUG', '-fsanitize=address,undefined',
           '-fno-sanitize-recover=all', '-I' + str(out),
           '-I' + str(ROOT / 'zephcore/adapters/bench/vendor'), str(HERE / 'compare.cpp'), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    result = json.loads(subprocess.check_output([str(exe)], text=True))
    if result['role_enum_bytes'] != (1 if args.short_enums else 4):
        raise RuntimeError(f"Unexpected stock role enum size: {result['role_enum_bytes']}")
    result.update(status='PASS', stock_commit=STOCK_COMMIT, short_enums=args.short_enums, input_sha256=PINS)
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
