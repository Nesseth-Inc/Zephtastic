#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual peer Position cache/export native checks; no device or network I/O."""
import argparse, hashlib, json, os, shlex, shutil, sys
from pathlib import Path
if not __debug__:
    raise SystemExit('Refusing disabled assertions')
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
APP = ROOT / 'mt_only'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--short-enums', action='store_true')
a = p.parse_args()
out = a.output.resolve()
if out == ROOT or ROOT in out.parents or out.exists():
    p.error('a fresh output directory outside the source tree is required')
sys.path.insert(0, str(HERE.parent / 'airtime_quality'))
from bounded_process import run_group, install_termination_handler
install_termination_handler()
proto = ROOT / 'zephcore/bench/upstream-differential/source'
vendor = ROOT / 'zephcore/adapters/bench/vendor'
mono = ROOT / 'zephcore/lib/monocypher'
inputs = list((APP / 'src').rglob('*.h')) + list((APP / 'src').rglob('*.hpp')) + [APP / 'src/Protobuf.cpp']
inputs += list(vendor.glob('*.h')) + [vendor / 'MtCodec.cpp', vendor / 'MtSignature.cpp']
inputs += list(proto.glob('*.pb.h')) + list((proto / 'nanopb').glob('*.[ch]'))
inputs += [mono / name for name in ('monocypher.c', 'monocypher.h', 'monocypher-ed25519.c', 'monocypher-ed25519.h')]
inputs += [HERE / 'cache_export_cases.cpp', HERE.parent / 'position_query_fixture.h', HERE.parent / 'endpoint_fixture.h', HERE.parent / 'airtime_quality/bounded_process.py', Path(__file__).resolve()]
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
pins = {str(path.relative_to(ROOT)): digest(path) for path in sorted(set(inputs))}
out.mkdir(parents=True)
(out / 'meshtastic').mkdir()
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header, out / 'meshtastic' / header.name)
flags = ['-fsanitize=address,undefined', '-g', '-O2', '-I' + str(APP / 'src'), '-I' + str(vendor), '-I' + str(mono), '-I' + str(out), '-I' + str(proto / 'nanopb')]
if a.short_enums:
    flags += ['-fshort-enums']
link = []
if sys.platform.startswith('linux'):
    flags += ['-DMT_HOST_OPENSSL=1']
    link += ['-lcrypto']
cc = shlex.split(os.environ.get('CC', 'cc'))
cxx = shlex.split(os.environ.get('CXX', 'c++'))
commands = []
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1', 'PYTHONDONTWRITEBYTECODE': '1'}
receipt = dict(status='FAIL', hardware=False, network=False, radio_io=False, short_enums=a.short_enums, ASAN_UBSAN=True, source_sha256=pins, commands=commands)
def run(argv, label, timeout):
    commands.append(dict(argv=[str(v) for v in argv], timeout_seconds=timeout, log=label))
    with (out / label).open('x') as stream:
        run_group([str(v) for v in argv], timeout=timeout, stdout=stream, env=env)
    commands[-1]['owned_process_cleanup'] = dict(leader_reaped=True, group_gone=True)
try:
    objects = []
    for source in [mono / 'monocypher.c', mono / 'monocypher-ed25519.c', *(proto / 'nanopb' / name for name in ('pb_common.c', 'pb_encode.c', 'pb_decode.c'))]:
        obj = out / (source.name + '.o')
        run([*cc, '-std=c11', *flags, '-c', source, '-o', obj], source.name + '.build.log', 90)
        objects.append(obj)
    exe = out / 'peer-position-tests'
    run([*cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, HERE / 'cache_export_cases.cpp', APP / 'src/Protobuf.cpp', vendor / 'MtCodec.cpp', vendor / 'MtSignature.cpp', *objects, *link, '-o', exe], 'build.log', 120)
    run([exe], 'test.log', 30)
    lines = (out / 'test.log').read_text().splitlines()
    if len([line for line in lines if line.startswith('PASS ')]) != 7 or lines[-1:] != ['PEER_POSITION_GROUPS 7']:
        raise RuntimeError('missing expected fixture acceptance groups')
    receipt['source_unchanged'] = all(digest(ROOT / path) == pin for path, pin in pins.items())
    if not receipt['source_unchanged']:
        raise RuntimeError('source changed during tests')
    receipt['status'] = 'PASS'
except BaseException as error:
    receipt['error'] = dict(type=type(error).__name__, detail=str(error), owned_process_cleanup=getattr(error, 'owned_process_cleanup', None))
finally:
    (out / 'SUMMARY.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({key: receipt[key] for key in ('status', 'hardware', 'radio_io', 'network', 'short_enums')}))
raise SystemExit(receipt['status'] != 'PASS')
