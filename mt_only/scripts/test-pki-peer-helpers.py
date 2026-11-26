#!/usr/bin/env python3
"""Strict peer parser/set fixtures with actual project crypto; no device I/O."""
import argparse, hashlib, json, os, platform, shlex, subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cases', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
app = Path(__file__).resolve().parents[1]
root = app.parent
cases = a.cases.resolve()
if not cases.is_relative_to(root) or not cases.is_file():
    p.error('Cases must be a regular source file in the exported source tree')
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'results.json').unlink(missing_ok=True)
vendor = root / 'zephcore/adapters/bench/vendor'
mono = root / 'zephcore/lib/monocypher'
inputs = sorted(set([cases, Path(__file__).resolve(),
                    *app.glob('src/pki/*.h'), *vendor.glob('*.h'),
                    vendor / 'MtSignature.cpp', vendor / 'MtCodec.cpp',
                    *mono.glob('*.h'), mono / 'monocypher.c', mono / 'monocypher-ed25519.c']))
def hashes():
    return {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
before = hashes()
flags = ['-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         *['-I' + str(f) for f in (app / 'src', app / 'src/pki',
                                  app / 'experimental/pki', vendor, mono)]]
link = []
if platform.system() == 'Linux':
    flags += ['-DMT_HOST_OPENSSL']
    link += ['-lcrypto']
cc = shlex.split(os.environ.get('CC', 'clang'))
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
commands = []
def run(command):
    commands.append(list(map(str, command)))
    subprocess.run(command, check=True)
objects = []
for name in ('monocypher.c', 'monocypher-ed25519.c'):
    obj = out / (name + '.o')
    run([*cc, '-std=c99', *flags, '-c', mono / name, '-o', obj])
    objects.append(obj)
run([*cxx, '-std=c++17', *flags, '-fno-exceptions', '-fno-rtti',
     '-c', cases, '-o', out / 'case-no-exceptions.o'])
exe = out / 'peer-helper-tests'
run([*cxx, '-std=c++17', *flags, cases, vendor / 'MtSignature.cpp',
     vendor / 'MtCodec.cpp', *objects, *link, '-o', exe])
r = subprocess.run([exe], text=True, capture_output=True, timeout=120,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
(out / 'test.log').write_text(r.stdout + r.stderr)
print(r.stdout + r.stderr, end='')
r.check_returncode()
if hashes() != before:
    raise RuntimeError('Source changed during validation')
report = dict(status='PASS', ASAN_UBSAN=True, no_exceptions_rtti_compile=True,
              source_stable=True, groups=sum(line.startswith('PASS ') for line in r.stdout.splitlines()),
              backend='pinned Monocypher/OpenSSL', hardware=False, synthetic_credentials_only=True,
              source_sha256=before)
(out / 'commands-private.json').write_text(json.dumps(commands, indent=2) + '\n')
(out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
