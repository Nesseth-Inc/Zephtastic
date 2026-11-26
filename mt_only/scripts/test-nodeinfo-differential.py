#!/usr/bin/env python3
"""Offline reply oracle: compile unchanged functions from pinned stock source."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--stock-source', type=Path, help='Also verify committed oracle slices against this Git checkout')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'results.json').unlink(missing_ok=True)
app = Path(__file__).resolve().parents[1]
repo = app.parent
proto = repo / 'zephcore/bench/upstream-differential/source'
reference = app / 'tests/nodeinfo-reference'
extract = runpy.run_path(str(repo / 'zephcore/bench/run-routing-differential.py'))['extract']
commit = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
for root in (proto, reference):
    lock = json.loads((root / 'source-lock.json').read_text())
    assert lock['commit'] == commit
    for item in lock['files']:
        assert hashlib.sha256((root / item['local_file']).read_bytes()).hexdigest() == item['sha256'], item

heartbeat = json.loads((reference / 'heartbeat-lock.json').read_text())
assert heartbeat['commit'] == commit
for item in heartbeat['slices']:
    local = (reference / item['file']).read_bytes()
    assert hashlib.sha256(local).hexdigest() == item['sha256']
    if args.stock_source:
        full = subprocess.check_output(['git', '-C', str(args.stock_source), 'show', commit + ':' + item['path']])
        assert b''.join(full.splitlines(keepends=True)[item['first'] - 1:item['last']]) == local, item
pieces, manifest = [], []
for path, markers in (
    (proto / 'NodeDB.cpp', ['NodeNum getFrom(', 'int8_t getHopsAway(']),
    (reference / 'RoutingModule.cpp', ['uint8_t RoutingModule::getHopLimitForResponse(']),
    (reference / 'MeshModule.cpp', ['void setReplyTo(']),
):
    for marker in markers:
        source, first, last = extract(path.read_text(), marker)
        pieces.append(source)
        manifest.append({'source': path.name, 'function': marker, 'first_line': first,
                         'last_line': last, 'compiled_sha256': hashlib.sha256(source.encode()).hexdigest(),
                         'transformation': 'none'})
(out / 'upstream-nodeinfo.inc').write_text('\n\n'.join(pieces) + '\n')
(out / 'slices.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header, out / 'meshtastic' / header.name)
vendor = repo / 'zephcore/adapters/bench/vendor'
mono = repo / 'zephcore/lib/monocypher'
flags = ['-fsanitize=address,undefined', '-g', '-I' + str(app / 'src'), '-I' + str(vendor),
         '-I' + str(mono), '-I' + str(out), '-I' + str(proto / 'nanopb')]
host_link = []
if sys.platform.startswith('linux'):
    flags += ['-DMT_HOST_OPENSSL=1']
    host_link += ['-lcrypto']
cc = shlex.split(os.environ.get('CC', 'clang'))
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
commands, objects = [], []
for src in [mono / 'monocypher.c', mono / 'monocypher-ed25519.c',
            *(proto / 'nanopb' / n for n in ['pb_common.c', 'pb_encode.c', 'pb_decode.c'])]:
    obj = out / (src.stem + '.o')
    cmd = [*cc, '-std=c11', *flags, '-c', str(src), '-o', str(obj)]
    subprocess.run(cmd, check=True)
    commands.append(cmd)
    objects.append(str(obj))
cmd = [*cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
       str(app / 'tests/nodeinfo_differential.cpp'), str(app / 'src/Protobuf.cpp'),
       str(vendor / 'MtCodec.cpp'), str(vendor / 'MtSignature.cpp'), *objects, *host_link,
       '-o', str(out / 'nodeinfo-differential')]
subprocess.run(cmd, check=True)
commands.append(cmd)
result = subprocess.run([str(out / 'nodeinfo-differential')], capture_output=True, text=True,
                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
(out / 'differential.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr)
result.check_returncode()
(out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
(out / 'results.json').write_text(json.dumps({
    'status': 'PASS', 'stock_commit': commit, 'ASAN_UBSAN': True, 'hardware': False,
    'reply_vectors': 144, 'heartbeat_sequences': 6, 'exact_stock_functions': 7, 'stock_phone_heartbeat_dispatch': True,
    'scope': 'Reply destination, request ID, ACK flag, stock response hops and canonical local User payload.',
    'limits': ['The stock module scheduler, signatures, RF and client UI are not executed.',
               'Stock return hops above the configured default are compared without a prototype clamp.',
               'Default configured stock hop limit is fixed to three; nodeDB and configuration are host fixtures.']
}, indent=2) + '\n')
