#!/usr/bin/env python3
"""Host-only, inert-by-default M139 role-policy fixture."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--run', action='store_true')
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if not a.run:
    p.error('inert by default; pass --run to compile and execute')
repo = a.source.resolve()
header = repo / 'mt_only/src/DeviceRolePolicy.h'
case = Path(__file__).resolve().with_name('m139_tak_role_tests.cpp')
out = a.output.resolve()
if not header.is_file() or not case.is_file():
    p.error('--source must be the M139 ZephTastic worktree')
if out.exists() or out.is_relative_to(repo):
    p.error('--output must be fresh and outside the source tree')
out.mkdir(parents=True)
cc = shlex.split(os.environ.get('CXX', 'clang++'))
inputs = {str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:' + x.name:
          hashlib.sha256(x.read_bytes()).hexdigest() for x in (header, case, Path(__file__).resolve())}
variants = []
for name, short in [('default', False), ('short-enums', True)]:
    for enabled in (False, True):
        mode = 'enabled' if enabled else 'disabled'
        directory = out / name / mode
        directory.mkdir(parents=True)
        binary = directory / 'm139-tak-role-tests'
        command = [*cc, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-g', '-O1', f'-I{repo / "mt_only/src"}']
        if short:
            command.append('-fshort-enums')
        if enabled:
            command.append('-DCONFIG_MT_TAK_TRACKER=1')
        command += [str(case), '-o', str(binary)]
        build = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1',
                                    'UBSAN_OPTIONS': 'halt_on_error=1'})
        (directory / 'build.log').write_text(shlex.join(command) + '\n' + build.stdout)
        if build.returncode:
            raise SystemExit(f'{name}/{mode} compile failed; see {directory / "build.log"}')
        run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1'})
        with (directory / 'build.log').open('a') as f:
            f.write(shlex.join([str(binary)]) + '\n' + run.stdout)
        if run.returncode:
            raise SystemExit(f'{name}/{mode} fixture failed; see {directory / "build.log"}')
        variants.append({'compiler_mode': name, 'tak_tracker_enabled': enabled, 'status': 'PASS',
                         'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                         'log_sha256': hashlib.sha256((directory / 'build.log').read_bytes()).hexdigest()})
if inputs != {str(x.relative_to(repo)) if x.is_relative_to(repo) else 'external:' + x.name:
              hashlib.sha256(x.read_bytes()).hexdigest() for x in (header, case, Path(__file__).resolve())}:
    raise SystemExit('SOURCE_CHANGED_DURING_TEST')
result = {'schema': 'm139-tak-role-native/v1', 'status': 'PASS', 'source_unchanged': True,
          'inputs_sha256': inputs, 'variants': variants, 'target_build': False, 'hardware': False, 'network': False}
(out / 'RESULT.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
print(json.dumps({'status': 'PASS', 'variants': len(variants), 'source_unchanged': True}))
