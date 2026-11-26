#!/usr/bin/env python3
"""Offline endpoint ACK oracle: compile unchanged functions from pinned stock source."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'results.json').unlink(missing_ok=True)
app = Path(__file__).resolve().parents[1]
repo = app.parent
sys.path.insert(0, str(repo))
import host_test_toolchain as host
proto = repo / 'zephcore/bench/upstream-differential/source'
reference = app / 'tests/nodeinfo-reference'
extract = runpy.run_path(str(repo / 'zephcore/bench/run-routing-differential.py'))['extract']
commit = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
for root in (proto, reference):
    lock = json.loads((root / 'source-lock.json').read_text())
    assert lock['commit'] == commit
    for item in lock['files']:
        assert hashlib.sha256((root / item['local_file']).read_bytes()).hexdigest() == item['sha256'], item

pieces, manifest = [], []
for path, markers in (
    (proto / 'NodeDB.cpp', ['NodeNum getFrom(', 'bool isFromUs(', 'bool isToUs(', 'bool isBroadcast(', 'int8_t getHopsAway(']),
    (reference / 'RoutingModule.cpp', ['uint8_t RoutingModule::getHopLimitForResponse(']),
    (proto / 'ReliableRouter.cpp', ['bool ReliableRouter::shouldSuccessAckWithWantAck(', 'void ReliableRouter::sniffReceived(']),
):
    for marker in markers:
        source, first, last = extract(path.read_text(), marker)
        pieces.append(source)
        manifest.append({'source': path.name, 'function': marker, 'first_line': first,
                         'last_line': last, 'compiled_sha256': hashlib.sha256(source.encode()).hexdigest(),
                         'transformation': 'none'})
(out / 'upstream-endpoint.inc').write_text('\n\n'.join(pieces) + '\n')
(out / 'slices.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header, out / 'meshtastic' / header.name)
vendor = repo / 'zephcore/adapters/bench/vendor'
mono = repo / 'zephcore/lib/monocypher'
flags = ['-fsanitize=address,undefined', '-g', '-I' + str(app / 'src'), '-I' + str(vendor),
         '-I' + str(mono), '-I' + str(out), '-I' + str(proto / 'nanopb'), *host.crypto_compile_flags()]
commands, objects = [], []
for src in [mono / 'monocypher.c', mono / 'monocypher-ed25519.c',
            *(proto / 'nanopb' / n for n in ['pb_common.c', 'pb_encode.c', 'pb_decode.c'])]:
    obj = out / (src.stem + '.o')
    cmd = [*host.cc(), '-std=c11', *flags, '-c', str(src), '-o', str(obj)]
    subprocess.run(cmd, check=True)
    commands.append(cmd)
    objects.append(str(obj))
cmd = [*host.cxx(), '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
       str(app / 'tests/endpoint_differential.cpp'), str(app / 'src/Protobuf.cpp'),
       str(vendor / 'MtCodec.cpp'), str(vendor / 'MtSignature.cpp'), *objects, *host.crypto_link_flags(),
       '-o', str(out / 'endpoint-differential')]
subprocess.run(cmd, check=True)
commands.append(cmd)
result = subprocess.run([str(out / 'endpoint-differential')], capture_output=True, text=True,
                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
(out / 'differential.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr)
result.check_returncode()
(out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
(out / 'results.json').write_text(json.dumps({
    'status': 'PASS', 'stock_commit': commit, 'ASAN_UBSAN': True, 'hardware': False,
    'stock_ack_decision_vectors': 1188, 'negative_and_resource_groups': 8,
    'exact_stock_functions': len(manifest),
    'scope': 'Endpoint ACK decisions against unchanged pinned ReliableRouter::sniffReceived; existing bounded endpoint queues exercised.',
    'limits': ['Stock module scheduling, RF and client UI are not executed.',
               'Only accepted Public TEXT and NodeInfo endpoint cases are compared.',
               'Return hops are capped at three and unmatched response packets remain deliberately silent.',
               'PKI proof, retransmission matching and NodeDB are environment stubs, not part of this oracle.']
}, indent=2) + '\n')
