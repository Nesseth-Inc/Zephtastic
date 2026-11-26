#!/usr/bin/env python3
"""Run synthetic M148 lifecycle and unchanged legacy storage tests, no devices."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--case', action='append', help='select a named test source from the fixed case list')
a = p.parse_args()
root, out = a.source.resolve(), a.output.resolve()
if out.exists() or out.is_relative_to(root):
    p.error('output must be fresh and outside source')
app = root / 'mt_only'
mono = root / 'zephcore/lib/monocypher'
vendor = root / 'zephcore/adapters/bench/vendor'
cases = ('m148_nonce_counter_tests.cpp', 'm148_replay_window_tests.cpp',
         'm148_lifecycle_command_tests.cpp', 'm148_storage_lifecycle_tests.cpp',
         'append_history_cases.cpp', 'usb_storage_command_cases.cpp')
if a.case:
    if len(set(a.case)) != len(a.case) or any(c not in cases for c in a.case):
        p.error('case selection must be unique and present in the fixed case list')
    cases = tuple(a.case)
cpp = [app / 'src/pki' / (name + '.cpp') for name in
       ('PkiIdentity', 'PkiLedger', 'PkiAppendHistory', 'PkiStorage')]
cpp.append(vendor / 'MtSignature.cpp')
c_sources = [mono / 'monocypher.c', mono / 'monocypher-ed25519.c']
inputs = sorted(set([*cpp, *c_sources, *(app / 'tests' / c for c in cases),
                     Path(__file__).resolve(),
                     *(app / 'src/pki').glob('*.h'),
                     *(app / 'experimental/pki').glob('*.h'),
                     *(vendor.glob('*.h')), *(mono.glob('*.h'))]))
def snapshot():
    return {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
before = snapshot()
out.mkdir(parents=True)
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti',
         *['-I' + str(d) for d in (app / 'src', app / 'src/pki', app / 'experimental/pki', vendor, mono)]]
cc, cxx = shlex.split(os.environ.get('CC', 'clang')), shlex.split(os.environ.get('CXX', 'clang++'))
rows, commands = [], []
error = None
def run(argv, log, timeout=180):
    argv = list(map(str, argv)); commands.append(argv)
    r = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    if r.returncode: raise RuntimeError('COMMAND_FAILED:' + str(r.returncode))
try:
    for label, extra in (('default', []), ('short-enums', ['-fshort-enums'])):
        folder = out / label; folder.mkdir()
        with (folder / 'compile.log').open('xb') as log:
            objects = []
            for i, source in enumerate([*c_sources, *cpp]):
                obj = folder / (str(i) + '.o')
                compiler = cc if i < len(c_sources) else cxx
                language = '-std=c11' if i < len(c_sources) else '-std=c++17'
                # RTTI is a C++ option, not applicable to C compilation.
                active = [f for f in flags if i >= len(c_sources) or f not in ('-fno-rtti', '-fno-exceptions')]
                run([*compiler, language, *active, *extra, '-c', source, '-o', obj], log)
                objects.append(obj)
            for case in cases:
                binary = folder / Path(case).stem
                run([*cxx, '-std=c++17', *flags, *extra, app / 'tests' / case,
                     *objects, '-o', binary], log)
                test_log = folder / (Path(case).stem + '.log')
                with test_log.open('xb') as stream:
                    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1',
                           'UBSAN_OPTIONS': 'halt_on_error=1'}
                    completed = subprocess.run([binary], stdout=stream, stderr=subprocess.STDOUT,
                                               env=env, timeout=180)
                rows.append({'case': case, 'variant': label, 'exit': completed.returncode,
                             'log_sha256': hashlib.sha256(test_log.read_bytes()).hexdigest(),
                             'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()})
                if completed.returncode: raise RuntimeError('CASE_FAILED:' + case + ':' + label)
except Exception as e:
    error = str(e)
stable = before == snapshot()
result = {'schema': 'zt-m148-root-lifecycle-native/v1',
          'status': 'PASS' if error is None and stable and len(rows) == 2 * len(cases) else 'FAIL',
          'error': error, 'source_unchanged': stable, 'source_sha256': before,
          'executions': rows, 'asan_ubsan': True, 'hardware': False, 'RF': False,
          'actual_NVS_driver': False, 'commands': commands}
(out / 'RESULT.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
print(json.dumps({k: result[k] for k in ('status', 'error', 'source_unchanged', 'executions')}))
raise SystemExit(0 if result['status'] == 'PASS' else 1)
