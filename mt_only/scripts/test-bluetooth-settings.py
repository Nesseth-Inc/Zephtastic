#!/usr/bin/env python3
"""Pure Bluetooth settings/journal/policy/session fixtures; no SDK, BLE or hardware."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys

if not __debug__:
    raise RuntimeError('Optimized Bluetooth qualification refused')
sys.dont_write_bytecode = True
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--short-enums', action='store_true')
a = p.parse_args()
app = Path(__file__).resolve().parents[1]
root = app.parent
vendor = root / 'zephcore/adapters/bench/vendor'
source = app / 'tests/bluetooth_settings_cases.cpp'
required = [app / 'src' / name for name in (
    'BluetoothSettings.h', 'BluetoothJournal.h', 'BluetoothPairingPolicy.h',
    'BlePairingEvent.h', 'SettingsJournal.h')]
if not all(path.is_file() and not path.is_symlink() for path in required):
    p.error('The five source-owned helper/backend headers must be integrated before validation')
out = a.output.resolve()
if out == root or root in out.parents or out.exists():
    p.error('Output must be a fresh path outside the source tree')


def pins():
    # Conservative transitive header/body membership prevents silent changes
    # while Root integrates new headers. No external checkout/cwd dependency.
    paths = {Path(__file__).resolve(), source}
    for directory in (app / 'src', vendor, root / 'zephcore/lib/monocypher'):
        paths.update(path for path in directory.rglob('*')
                     if path.is_file() and path.suffix in ('.h', '.hpp', '.inc'))
    result = {}
    for path in sorted(paths):
        if path.is_symlink() or not path.is_file():
            raise RuntimeError('Ordinary source input required')
        raw = path.read_bytes()
        result[path.relative_to(root).as_posix()] = {
            'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
            'mode': path.stat().st_mode & 0o777}
    return result


before = pins()
out.mkdir(parents=True, exist_ok=False)
commands = []
flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
         *(['-fshort-enums'] if a.short_enums else []),
         '-I' + str(app / 'src'), '-I' + str(vendor)]
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
if not cxx:
    raise RuntimeError('A host C++ compiler is required')
binary = out / 'bluetooth-settings-tests'


def compile_stage(name, command):
    commands.append(list(map(str, command)))
    with (out / name).open('x') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=90)
    if pins() != before:
        raise RuntimeError('Source changed during Bluetooth fixture compilation')


compile_stage('compile.log', [*cxx, *flags, str(source), '-o', str(binary)])
compile_stage('no-exceptions.log', [*cxx, *flags, '-fno-exceptions', '-fno-rtti',
                                  '-c', str(source), '-o', str(out / 'no-exceptions.o')])
commands.append([str(binary)])
result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30,
                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0',
                             'UBSAN_OPTIONS': 'halt_on_error=1'})
(out / 'tests.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
result.check_returncode()
if pins() != before:
    raise RuntimeError('Source changed during Bluetooth fixture execution')
groups = [line for line in result.stdout.splitlines() if line.startswith('PASS ')]
metadata = [line for line in result.stdout.splitlines()
            if line.startswith('BLUETOOTH_TEST_METADATA ')]
if len(groups) != 14 or len(metadata) != 1:
    raise RuntimeError('Exact Bluetooth fixture groups/metadata absent')
fields = dict(word.split('=', 1) for word in metadata[0].split()[1:])
if set(fields) != {'groups', 'record_size', 'codec_size', 'corruption_bits', 'torn_cut_points'}:
    raise RuntimeError('Unknown Bluetooth fixture metadata')
if not all(value.isascii() and value.isdecimal() for value in fields.values()):
    raise RuntimeError('Noncanonical Bluetooth fixture metadata')
counts = {key: int(value) for key, value in fields.items()}
if counts != {'groups': 14, 'record_size': 32, 'codec_size': 12,
              'corruption_bits': 256, 'torn_cut_points': 32}:
    raise RuntimeError('Bluetooth fixture coverage or geometry drift')
(out / 'RESULT.json').write_text(json.dumps({
    'schema': 'zt-bluetooth-settings-host-tests/v1', 'status': 'PASS',
    'groups': len(groups), 'ASAN_UBSAN': True, 'short_enums': a.short_enums,
    'no_exceptions_rtti_compile': True, 'source_unchanged': True,
    'source_pins': before, 'commands': commands, 'counts': counts,
    'common_contract_sha256': '207d9c098ed64e1e2d246946ca0faa0447f584e6f8bafd70c06b2a3277834810',
    'hardware': False, 'RF': False, 'SDK': False, 'Zephyr_runtime': False,
    'PIN_values': 'public synthetic fixtures only; no actual passkeys read',
    'limits': [
        'Pure helper/backend fault model only; no actual BLE pairing, bond persistence or negotiated security proof.',
        'Pairing event callers must hold the existing session lock; no callback concurrency or UI rendering is exercised.',
        'Journal tests model independent mtble policy records; Zephyr-owned keys and application-wide atomicity are outside scope.'
    ]}, sort_keys=True, indent=2) + '\n')
