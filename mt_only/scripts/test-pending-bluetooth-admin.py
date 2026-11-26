#!/usr/bin/env python3
"""Pure RAM lifecycle fixture; no radio, SDK, storage or transport qualification."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

if not __debug__:
    raise RuntimeError('Optimized fixture refused')
app = Path(__file__).resolve().parents[1]
root = app.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--short-enums', action='store_true')
a = p.parse_args()
out = a.output.resolve()
if out.exists() or out == root or root in out.parents:
    p.error('Fresh output outside source required')
out.mkdir(mode=0o700, parents=True)
selected = [Path(__file__).resolve(), app/'tests/pending_bluetooth_admin_tests.cpp',
            app/'src/PendingBluetoothAdmin.h', app/'src/BluetoothSettings.h']
def pins():
    return {str(f.relative_to(root)): {'bytes': f.stat().st_size,
            'sha256': hashlib.sha256(f.read_bytes()).hexdigest()} for f in selected}
before = pins()
compiler = os.environ.get('CXX', 'clang++')
version = subprocess.run([compiler, '--version'], capture_output=True, timeout=30, check=True)
(out/'compiler.txt').write_bytes(version.stdout+version.stderr)
argv = [compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
        '-fno-rtti', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I'+str(app/'src'), str(app/'tests/pending_bluetooth_admin_tests.cpp'),
        '-o', str(out/'fixture')]
if a.short_enums:
    argv.insert(1, '-fshort-enums')
compiled = subprocess.run(argv, capture_output=True, timeout=60)
(out/'compile.log').write_bytes(compiled.stdout+compiled.stderr)
ran = None
if compiled.returncode == 0:
    ran = subprocess.run([str(out/'fixture')], capture_output=True, timeout=30)
    (out/'run.log').write_bytes(ran.stdout+ran.stderr)
stable = before == pins()
passed = compiled.returncode == 0 and ran is not None and ran.returncode == 0 and stable
result = {'status': 'PASS_PURE_PENDING_LIFECYCLE' if passed else 'FAIL',
          'command': argv, 'short_enums': a.short_enums, 'compile_returncode': compiled.returncode,
          'run_returncode': None if ran is None else ran.returncode,
          'source_stable': stable, 'files': before, 'root_executed': True,
          'production_caller_or_persistence_or_transport_proof': False,
          'hardware_or_firmware_qualification': False}
(out/'RESULT.json').write_text(json.dumps(result, sort_keys=True, indent=2)+'\n')
print(result['status'])
raise SystemExit(0 if passed else 1)
