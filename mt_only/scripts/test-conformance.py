#!/usr/bin/env python3
"""Independent conformance cases against a selected checkout; no device IO.

Unlike a green baseline suite, known parity gaps return failure. The JSON keeps
each passing/failing case explicit, so a failure is not mislabeled as success.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--output', type=Path, required=True)
p.add_argument('--journal', action='store_true', help='test the M5 settings journal contract')
a = p.parse_args()
root = a.source.resolve()
sys.path.insert(0, str(root))
import host_test_toolchain as host
import source_provenance as provenance
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'
mono = root / 'zephcore/lib/monocypher'
(out / 'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header, out / 'meshtastic' / header.name)
flags = ['-fsanitize=address,undefined', '-g', '-I' + str(root / 'mt_only/src'),
         '-I' + str(vendor), '-I' + str(mono), '-I' + str(out),
         '-I' + str(proto / 'nanopb'), *host.crypto_compile_flags()]
commands = []
objects = []
def source_state():
    files = [p for folder in [root / 'mt_only/src', vendor, mono, proto]
             for p in folder.rglob('*') if p.is_file() and p.suffix in ('.h', '.cpp', '.c')]
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(files)}

before = source_state()
source_info = provenance.source_provenance(root)
test_root = Path(__file__).resolve().parents[1]
test_paths = [Path(__file__).resolve(), test_root / 'tests/conformance_fixture.h',
              test_root / 'tests/conformance_tests.cpp', test_root / 'tests/journal_conformance_tests.cpp']
test_hashes = {str(p.relative_to(test_root)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in test_paths}
for src in [mono / 'monocypher.c', mono / 'monocypher-ed25519.c',
            *(proto / 'nanopb' / n for n in ['pb_common.c', 'pb_encode.c', 'pb_decode.c'])]:
    obj = out / (src.stem + '.o')
    cmd = [*host.cc(), '-std=c11', *flags, '-c', str(src), '-o', str(obj)]
    subprocess.run(cmd, check=True)
    commands.append(cmd)
    objects.append(str(obj))
test = Path(__file__).resolve().parents[1] / ('tests/journal_conformance_tests.cpp' if a.journal else 'tests/conformance_tests.cpp')
cmd = [*host.cxx(), '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
       str(test), str(root / 'mt_only/src/Protobuf.cpp'), str(vendor / 'MtCodec.cpp'),
       str(vendor / 'MtSignature.cpp'), *objects, *host.crypto_link_flags(), '-o', str(out / 'conformance-tests')]
subprocess.run(cmd, check=True)
commands.append(cmd)
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'}
r = subprocess.run([str(out / 'conformance-tests')], text=True, capture_output=True, env=env)
(out / 'conformance.log').write_text(r.stdout + r.stderr)
(out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
cases = [{'name': line.split(' ', 1)[1], 'status': line.split(' ', 1)[0]}
         for line in r.stdout.splitlines() if line.startswith(('PASS ', 'FAIL '))]
after = source_state()
stable = before == after and source_info == provenance.source_provenance(root)
report = {**source_info, 'status': 'PASS' if r.returncode == 0 and stable else 'FAIL',
          'source_stable_during_run': stable,
          'hardware': False, 'ASAN_UBSAN': True, 'cases': cases,
          'test_source_sha256': test_hashes, 'exit_code': r.returncode if stable else 2}
(out / 'source-sha256.json').write_text(json.dumps(before, indent=2) + '\n')
(out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
print(r.stdout + r.stderr, end='')
raise SystemExit(r.returncode if stable else 2)
