#!/usr/bin/env python3
"""Run bounded StoreForward policy/codec/storage fixtures under host sanitizers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / 'mt_only'
PROTO = ROOT / 'zephcore/bench/upstream-differential/source'
NANOPB = PROTO / 'nanopb'
SUITES = ('archive', 'wire', 'control', 'memory', 'packet', 'settings')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    files = [APP / ('src/modules/StoreForward' + name) for name in ('Archive.h', 'Control.h', 'Wire.h', 'Wire.cpp', 'Memory.h', 'Memory.cpp', 'Packet.h', 'Settings.h', 'Journal.h')]
    files += [APP / f'tests/store_forward_{name}_tests.cpp' for name in SUITES]
    files += [Path(__file__).resolve(), APP / 'src/Protobuf.cpp']
    files += sorted(PROTO.glob('*.pb.h')) + sorted(NANOPB.glob('*.c')) + sorted(NANOPB.glob('*.h'))
    pins = {str(p.relative_to(ROOT)): digest(p) for p in files}
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    cc = shlex.split(os.environ.get('CC', 'clang'))
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    commands, runs = [], []

    def run(argv, label):
        result = subprocess.run(list(map(str, argv)), cwd=ROOT, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (out / (label + '.log')).write_bytes(result.stdout)
        commands.append({'label': label, 'argv': list(map(str, argv)), 'exit': result.returncode})
        if result.returncode:
            raise RuntimeError(f'{label} failed: see retained log')
        return result.stdout.decode(errors='replace')

    failure = None
    try:
        with tempfile.TemporaryDirectory(prefix='zt-store-forward-core-') as temporary:
            build = Path(temporary)
            generated = build / 'meshtastic'
            generated.mkdir()
            for header in PROTO.glob('*.pb.h'):
                shutil.copyfile(header, generated / header.name)
            common = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                      '-Wall', '-Wextra', '-Werror', '-DMT_STORE_FORWARD_MEMORY_NATIVE_TEST=1', '-I' + str(build),
                      '-I' + str(APP / 'src'), '-I' + str(APP / 'src/modules'),
                      '-I' + str(ROOT / 'zephcore/adapters/bench/vendor'),
                      '-I' + str(NANOPB), '-I' + str(PROTO)]
            for variant, enum_flags in (('default', []), ('short', ['-fshort-enums'])):
                objects = []
                for name in ('pb_common', 'pb_encode', 'pb_decode'):
                    obj = build / f'{variant}-{name}.o'
                    run([*cc, '-std=c11', *common, *enum_flags, '-c', NANOPB / (name + '.c'), '-o', obj],
                        f'{variant}-{name}-compile')
                    objects.append(obj)
                for name, source in (('protobuf', APP / 'src/Protobuf.cpp'),
                                     ('wire', APP / 'src/modules/StoreForwardWire.cpp')):
                    obj = build / f'{variant}-{name}.o'
                    run([*cxx, '-std=c++17', *common, *enum_flags, '-c', source, '-o', obj],
                        f'{variant}-{name}-compile')
                    objects.append(obj)
                for suite in SUITES:
                    for enabled in ((True, False) if suite == 'memory' else (True,)):
                        label = f'{variant}-{suite}' + ('-off' if not enabled else '')
                        binary = build / label
                        additional = [APP / 'src/modules/StoreForwardMemory.cpp'] if suite == 'memory' else []
                        defines = ['-DCONFIG_MT_STORE_FORWARD_PSRAM=1'] if enabled else []
                        run([*cxx, '-std=c++17', *common, *enum_flags, *defines,
                             APP / f'tests/store_forward_{suite}_tests.cpp', *additional,
                             *objects, '-o', binary], label + '-compile')
                        binary_sha = digest(binary)
                        output = run([binary], label + '-run')
                        runs.append({'suite': suite, 'variant': variant, 'feature': enabled,
                                     'binary_sha256': binary_sha, 'output': output.splitlines()})
    except Exception as error:
        failure = f'{type(error).__name__}: {error}'
    stable = all(digest(ROOT / name) == value for name, value in pins.items())
    result = {'status': 'PASS' if failure is None and stable and len(runs) == 14 else 'FAIL',
              'source_pins': pins, 'source_stable': stable, 'runs': runs,
              'commands': commands, 'error': failure, 'target': False, 'hardware': False,
              'leak_sanitizer': 'disabled; host support unavailable; ASan and UBSan enabled'}
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'runs': len(runs), 'error': failure, 'output': str(out)}))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
