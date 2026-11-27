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


def run_owner(build, proto, env):
    main = (APP/'src/main.cpp').read_text()
    methods = []
    for start in ('static void checkpointHistories(', 'static bool administrationWorkPending(',
                  'static void pollBackgroundHistories('):
        i = main.index(start)
        methods.append(main[i:main.index('\n}',i)+2])
    (build/'owner.inc').write_text('\n'.join(methods))
    if 'pollBackgroundHistories(hotNow,clientIngressSaturated);' not in main[main.index('int main() {'):]:
        raise ValueError('Owner fixture is not wired to the application loop')
    mono = ROOT/'zephcore/lib/monocypher'
    vendor = ROOT/'zephcore/adapters/bench/vendor'
    flags = ['-O1', '-UNDEBUG', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-DCONFIG_MT_RF_ADMIN_GET=1', '-DCONFIG_MT_HOP_HISTORY=1',
             *['-I'+str(p) for p in (build, APP/'src', APP/'tests', mono, vendor, proto/'nanopb')]]
    if sys.platform == 'darwin':
        flags.append('-DEKEYREJECTED=129')
    objects = []
    for src in [mono/'monocypher.c', mono/'monocypher-ed25519.c',
                *[proto/'nanopb'/f'pb_{s}.c' for s in ('common','encode','decode')]]:
        obj = build/(src.stem+'.o')
        subprocess.run([*host.cc(), '-std=c11', *flags, '-c', src, '-o', obj], check=True)
        objects.append(obj)
    exe = build/'owner'
    subprocess.run([*host.cxx(), '-std=c++17', *flags, *host.crypto_compile_flags(), HERE/'owner.cpp',
                    APP/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',
                    *objects,*host.crypto_link_flags(),'-o',exe], check=True)
    subprocess.run([exe], env=env, check=True)


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
    pins = {**fixture.PINS, 'meshUtils.h': '1ab754c75cda9663380181870a4d52f53058cdb1449f08522007d9d26a43fcf0'}
    for name, expected in pins.items():
        data = (args.stock / 'src' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'{name} differs from {fixture.STOCK}')
        sources[name] = data.decode()
    report = {'stock_revision': fixture.STOCK, 'input_sha256': pins, 'runs': [],
              'hardware': False, 'asan_ubsan': True, 'nvs': {'status': 'NOT_RUN'}}
    with tempfile.TemporaryDirectory(prefix='hop-restart-') as tmp:
        build = Path(tmp)
        fixture.prepare(build, sources)
        helper = fixture.method(sources['meshUtils.h'], 'template <typename T> constexpr bool is_pow_of_2')
        (build / 'meshUtils.h').write_text('#pragma once\n'+helper+'\n')
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
        run_owner(build, proto, env)
        report['outstanding_admin_query'] = 'PASS'
        if args.zephyr:
            lock = next(x for x in json.loads((APP/'dependency-lock.json').read_text()) if x['path']=='zephyr')
            git = ['git','-C',str(args.zephyr)]
            head = subprocess.check_output([*git,'rev-parse','HEAD'],text=True).strip()
            if head != lock['commit']:
                raise ValueError('Zephyr HEAD differs from the dependency lock')
            for name in ('subsys/kvss/nvs/nvs_priv.h', 'include/zephyr/kvss/nvs.h'):
                original = subprocess.check_output([*git,'cat-file','blob',f'{head}:{name}'])
                if (args.zephyr/name).read_bytes() != original:
                    raise ValueError(f'{name} differs from the pinned blob')
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
