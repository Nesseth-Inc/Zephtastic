#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare warm and cold boot with pinned Meshtastic; --zephyr includes both real NVS geometries."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
ROOT = APP.parent
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--zephyr', type=Path, default=os.getenv('ZEPHYR_BASE'),
                        help='patched pinned Zephyr checkout, including the native NVS engine')
    args = parser.parse_args()
    args.output.unlink(missing_ok=True)
    spec = importlib.util.spec_from_file_location('hop_fixture', HERE.parent / 'stock_hop_beacon/run.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    sources = {}
    for name, expected in fixture.PINS.items():
        data = (args.stock / 'src' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'{name} differs from {fixture.STOCK}')
        sources[name] = data.decode()
    report = {'stock_revision': fixture.STOCK, 'input_sha256': fixture.PINS, 'runs': [],
              'hardware': False, 'asan_ubsan': True, 'nvs': {'status': 'NOT_RUN'}}
    with tempfile.TemporaryDirectory(prefix='hop-restart-') as tmp:
        build = Path(tmp)
        fixture.prepare(build, sources)
        (build / 'FSCommon.h').write_text('#include "StockFilesystem.h"\n')
        proto = ROOT / 'zephcore/bench/upstream-differential/source'
        (build / 'meshtastic').symlink_to(proto, target_is_directory=True)
        env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
        for label, abi in [('normal', []), ('short', ['-fshort-enums'])]:
            exe = build / label
            subprocess.run([*host.cxx(), '-std=c++17', '-O1', '-UNDEBUG', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-DHAS_VARIABLE_HOPS=1', '-DPIO_UNIT_TESTING',
                            '-DCONFIG_MT_HOP_HISTORY=1', *abi,
                            *['-I'+str(p) for p in (build, HERE, APP/'src', proto/'nanopb')],
                            HERE/'cases.cpp', build/'HopScalingModule.cpp', '-o', exe], check=True)
            row = json.loads(subprocess.check_output([exe], env=env, text=True))
            if row['status'] != 'PASS' or row['compared_boots'] != 4 or row['hourly_saves'] != 7:
                raise ValueError(f'Incomplete donor run: {row}')
            report['runs'].append({'enum': label, **row})
        if args.zephyr:
            lock = next(x for x in json.loads((APP/'dependency-lock.json').read_text()) if x['path']=='zephyr')
            for entry in lock['overlay']:
                name = entry['path']
                if name.startswith(('subsys/kvss/nvs/', 'subsys/settings/src/settings_')):
                    if hashlib.sha256((args.zephyr/name).read_bytes()).hexdigest() != entry['sha256']:
                        raise ValueError(f'{name} differs from the pinned Zephyr source')
            report['zephyr_revision'] = lock['commit']
            out = build / 'nvs'
            subprocess.run([sys.executable, HERE.parent/'pki_storage_nvs/run.py', '--zephyr', args.zephyr,
                            '--output', out, '--cases', HERE/'nvs.cpp'], check=True)
            result = json.loads((out/'RESULTS.json').read_text())
            runs = []
            for sectors, keys, cuts, collections, erases in re.findall(
                    r'^HOP_NVS (\d+) (\d+) (\d+) (\d+) (\d+)$', (out/'run.log').read_text(), re.M):
                runs.append(dict(sectors=int(sectors), protected_keys=int(keys), interruption_cases=int(cuts),
                                 checkpoints_with_collection=int(collections), erases_in_120_checkpoints=int(erases)))
            if result['status'] != 'PASS' or not result['source_stable'] or [x['sectors'] for x in runs] != [4, 8]:
                raise ValueError('Incomplete NVS qualification')
            report['nvs'] = {'status': 'PASS', 'runs': runs,
                             'zephyr_sha256': {k:v for k,v in result['source_sha256'].items()
                                              if k.startswith(('subsys/', 'include/zephyr/'))}}
    report['status'] = 'PASS'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'runs':report['runs'], 'nvs':report['nvs']}))


if __name__ == '__main__':
    main()
