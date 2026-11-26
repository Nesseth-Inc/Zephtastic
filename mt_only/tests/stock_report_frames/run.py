#!/usr/bin/env python3
"""Check the pinned screen fields and run a bounded live-owner fixture."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock',required=True,type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root))
import host_test_toolchain as host
pin='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
def donor(group):
    return subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:src/modules/Telemetry/{group}Telemetry.cpp'],text=True)
env=donor('Environment')
fields={'Environment':'temperature relative_humidity barometric_pressure iaq voltage current lux white_lux weight distance radiation',
        'AirQuality':'pm10_standard pm25_standard pm100_standard co2 form_formaldehyde',
        'Power':'ch1_voltage ch2_current ch3_current','Health':'temperature heart_bpm spO2'}
for group,names in fields.items():
    frame=donor(group).split('::drawFrame(',1)[1].split('\n}',1)[0]
    for name in names.split():
        assert name in frame,(group,name)
assert 'return !shouldKeepCurrentRemoteDisplay();' in env
assert 'DisplaySource::FavoriteNodesOnly:\n        return false;' in env
with tempfile.TemporaryDirectory() as temp:
    out=Path(temp)
    proto=root/'zephcore/bench/upstream-differential/source'
    (out/'meshtastic').symlink_to(proto,target_is_directory=True)
    for flags in ([],['-fshort-enums']):
        subprocess.run([*host.cxx(),'-std=c++17','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*flags,
                        '-I'+str(out),'-I'+str(root/'mt_only/src'),'-I'+str(proto/'nanopb'),Path(__file__).with_name('cases.cpp'),
                        '-o',out/'cases'],check=True)
        subprocess.run([out/'cases'],check=True)
