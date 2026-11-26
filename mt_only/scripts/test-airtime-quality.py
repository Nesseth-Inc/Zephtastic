#!/usr/bin/env python3
"""M60 measured TX airtime integration checks. No hardware or RF access."""
import argparse
import datetime
import hashlib
import json
import os
import runpy
from pathlib import Path
import shutil
import sys

if not __debug__:
    raise SystemExit('Refusing disabled assertions')

app = Path(__file__).resolve().parents[1]
source = app.parent
fixture = app / 'tests/airtime_quality'
sys.path.insert(0, str(source))
from host_test_toolchain import cc, cxx
sys.path.insert(0, str(fixture))
from bounded_process import install_termination_handler

install_termination_handler()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--short-enums', action='store_true')
parser.add_argument('--fixture', choices=('accounting', 'meter-quality', 'window-math', 'actual-responder', 'local-telemetry'), required=True)
args = parser.parse_args()
out = args.output.resolve()
if out.is_relative_to(source):
    parser.error('output must be outside the source tree')
out.mkdir(parents=True, exist_ok=True)
manifest_path = out / 'INPUT-MANIFEST.json'
if manifest_path.exists():
    parser.error('output already contains an airtime test invocation')

def hashes(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob('*') if path.is_file()
            and '.git' not in path.parts and '__pycache__' not in path.parts}

def compiler(command):
    if not command or not shutil.which(command[0]):
        parser.error('compiler unavailable')
    path = Path(shutil.which(command[0])).resolve()
    return path, [str(path), *command[1:]]

cc_path, cc_argv = compiler(cc())
cxx_path, cxx_argv = compiler(cxx())
tools = {'cc': cc_path, 'cxx': cxx_path, 'python': Path(sys.executable).resolve()}
argv = {'cc': cc_argv, 'cxx': cxx_argv, 'python': [str(tools['python'])]}
manifest = {'scope': 'source-owned offline integration test snapshot',
            'source_sha256': hashes(source), 'independent_sha256': hashes(fixture),
            'toolchain': {name: {'path': str(path), 'argv': argv[name], 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                          for name, path in tools.items()}}
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
manifest_digest = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
compile_out = out / 'compile'
command = [sys.executable, '-B', str(fixture / 'qualify.py'), '--source', str(source),
           '--output', str(compile_out), '--fixture', args.fixture,
           '--input-manifest', str(manifest_path), '--input-manifest-sha256', manifest_digest]
if args.short_enums:
    command.append('--short-enums')
receipt = {'status': 'FAIL', 'hardware': False, 'RF': False,
           'fixture': args.fixture, 'short_enums': args.short_enums,
           'input_manifest_sha256': manifest_digest, 'commands': [command],
           'driver_execution': 'in-process runpy; fixture owns bounded child groups'}
def run_fixture(command, log):
    # Each fixture handles SIGTERM and owns its compiler/test groups. Running it
    # in this process avoids a nested launcher killing that owner prematurely.
    from contextlib import redirect_stdout, redirect_stderr
    saved_argv = sys.argv
    sys.argv = [command[2], *command[3:]]
    try:
        with redirect_stdout(log), redirect_stderr(log):
            try:
                runpy.run_path(command[2], run_name='__main__')
            except SystemExit as result:
                if result.code not in (None, 0):
                    raise RuntimeError('Fixture exited unsuccessfully: ' + str(result.code)) from result
    finally:
        sys.argv = saved_argv

try:
    with (out / 'compile-driver.log').open('w') as log:
        run_fixture(command, log)
    compiled = json.loads((compile_out / 'SUMMARY.json').read_text())
    receipt['compile_receipt_sha256'] = hashlib.sha256((compile_out / 'SUMMARY.json').read_bytes()).hexdigest()
    if args.fixture in ('accounting', 'meter-quality', 'window-math'):
        assert compiled['status'] == 'PASS'
        receipt['groups'] = compiled['groups']
    else:
        assert compiled['status'] == 'COMPILED_NOT_RUN'
        client = [sys.executable, '-B', str(fixture / 'test_actual_stock_responder.py'),
                  '--executable', compiled['executable'], '--output', str(out / 'stock-client'),
                  '--compile-receipt', str(compile_out / 'SUMMARY.json')]
        if args.fixture == 'local-telemetry':
            client.append('--local')
        receipt['commands'].append(client)
        with (out / 'stock-driver.log').open('w') as log:
            run_fixture(client, log)
        result = json.loads((out / 'stock-client/SUMMARY.json').read_text())
        assert result['status'] == 'PASS' and len(result['cases']) == 56
        receipt['stock_cases'] = len(result['cases'])
        receipt['stock_receipt_sha256'] = hashlib.sha256((out / 'stock-client/SUMMARY.json').read_bytes()).hexdigest()
    receipt['status'] = 'PASS'
except BaseException as error:
    receipt['error_type'] = type(error).__name__
    if hasattr(error, 'owned_process_cleanup'):
        receipt['owned_process_cleanup'] = error.owned_process_cleanup
finally:
    receipt['source_unchanged'] = hashes(source) == manifest['source_sha256']
    receipt['fixture_unchanged'] = hashes(fixture) == manifest['independent_sha256']
    receipt['input_manifest_unchanged'] = hashlib.sha256(manifest_path.read_bytes()).hexdigest() == manifest_digest
    receipt['toolchain_unchanged'] = all(hashlib.sha256(path.read_bytes()).hexdigest() == manifest['toolchain'][name]['sha256']
                                         for name, path in tools.items())
    if not all(receipt[name] for name in ('source_unchanged', 'fixture_unchanged', 'input_manifest_unchanged', 'toolchain_unchanged')):
        receipt['status'] = 'FAIL'
    receipt['ended_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (out / 'SUMMARY.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt))
raise SystemExit(receipt['status'] != 'PASS')
