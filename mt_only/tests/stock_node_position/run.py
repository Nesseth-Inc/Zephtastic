#!/usr/bin/env python3
"""Compare NodeInfo and Position policies with Meshtastic's pinned implementation."""
from pathlib import Path
import argparse
import json
import re
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
TEST = Path(__file__).resolve().parent
PROTO = ROOT / 'zephcore/bench/upstream-differential/source'
PIN = 'd5f16191ca28b01b5024f590f04a67f25d66f1b1'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', required=True, type=Path)
parser.add_argument("--output", type=Path, help="Write the executed counts per enum ABI")
args = parser.parse_args()

def donor(path):
    return subprocess.check_output(['git','-C',args.stock,'show',f'{PIN}:src/{path}'],text=True)

def method(source, signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]

node=donor('modules/NodeInfoModule.cpp')
position=donor('modules/PositionModule.cpp')
precision=donor('mesh/PositionPrecision.cpp')
# Keep the donor's entire allocation decision. Its output packet is captured by a stub.
methods=[method(node,'meshtastic_MeshPacket *NodeInfoModule::allocReply('),
         method(precision,'int32_t truncateCoordinate(int32_t coordinate, uint32_t precision)'),
         method(position,'meshtastic_MeshPacket *PositionModule::allocReply('),
         method(position,'bool PositionModule::positionWithinPrecisionCell('),
         method(position,'uint32_t PositionModule::effectiveBroadcastIntervalMs(')]
defaults = donor('mesh/Default.h')
for name in ('default_traffic_mgmt_position_min_interval_secs', 'default_position_stationary_broadcast_secs'):
    methods.append(next(line for line in defaults.splitlines() if line.startswith('#define '+name+' ')))
first=node.index('bool requestWantResponse = ')
methods.append('bool wantRepliesForRole(bool wantReplies){'+node[first:node.index(';',first)+1]+'return requestWantResponse;}')
alloc=method(position,'meshtastic_MeshPacket *PositionModule::allocPositionPacket(')
fields=alloc[alloc.index('    if (pos_flags &'):alloc.index('    LOG_INFO("Position packet')]
methods.append('meshtastic_Position emitFields(const meshtastic_Position &localPosition, uint32_t pos_flags){meshtastic_Position p{};'+fields+'return p;}')
with tempfile.TemporaryDirectory(prefix='zt-stock-node-position-') as tmp:
    tmp=Path(tmp)
    (tmp/'meshtastic').mkdir()
    for f in PROTO.glob('*.pb.h'): (tmp/'meshtastic'/f.name).symlink_to(f)
    (tmp/'oracle.inc').write_text('\n'.join(methods))
    counts = {}
    for mode in ([], ['-fshort-enums']):
        compiler=shlex.split(os.environ.get('CXX','c++'))
        exe=tmp/'node-position-check'
        subprocess.run([*compiler,'-std=c++17','-O1','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer',*mode,'-I',tmp,'-I',PROTO/'nanopb',
                        '-I',ROOT/'mt_only/src',TEST/'cases.cpp','-o',exe],check=True)
        completed = subprocess.run([exe], check=True, text=True, stdout=subprocess.PIPE,
                                   env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
        print(completed.stdout, end="")
        row = re.fullmatch(r"PASS (\d+) NodeInfo admission/role and (\d+) Position reply/precision/measurement cases\s*", completed.stdout)
        if row is None or min(map(int, row.groups())) <= 0:
            raise ValueError("Missing NodeInfo or Position comparisons")
        counts["short" if mode else "native"] = {"node_info": int(row[1]), "position": int(row[2])}
    if args.output:
        args.output.write_text(json.dumps({"stock_revision": PIN, "comparisons": counts}, indent=2) + "\n")
