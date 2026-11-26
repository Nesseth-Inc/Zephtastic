#!/usr/bin/env python3
"""Compile the production GATT callbacks and wire encoder with pinned nanopb."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--short-enums',action='store_true')
a=p.parse_args()
app=Path(__file__).resolve().parents[2]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
out=a.output.resolve()
out.mkdir(parents=True,exist_ok=False)
proto=root/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir()
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header,out/'meshtastic'/header.name)
for name in ('kernel.h','bluetooth/bluetooth.h','bluetooth/conn.h','bluetooth/gatt.h','sys/atomic.h','sys/byteorder.h'):
    header=out/'zephyr'/name
    header.parent.mkdir(parents=True,exist_ok=True)
    header.write_text('#include "ble_zephyr_mock.h"\n')
flags=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
       '-I'+str(out),'-I'+str(proto/'nanopb'),'-I'+str(app/'tests')]
if a.short_enums:
    flags.append('-fshort-enums')
objects=[]
for source in ('pb_common','pb_decode','pb_encode'):
    obj=out/(source+'.o')
    subprocess.run([*host.cc(),*flags,'-std=c11','-c',proto/'nanopb'/(source+'.c'),'-o',obj],check=True)
    objects.append(obj)
cpp=app/'src/BleTransport.cpp'
assert 'BT_UUID_128_ENCODE(0x5a3d6e49,0x06e6,0x4423,0x9944,0xe9de8cdf9547)' in cpp.read_text()
subprocess.run([*host.cxx(),*flags,'-std=c++17','-pthread','-DCONFIG_MT_DISPLAY=1','-DCONFIG_BT_APP_PASSKEY=1',
                '-DCONFIG_MT_BLE_LOG_RADIO=1',Path(__file__).with_name('cases.cpp'),*objects,'-o',out/'cases'],check=True)
subprocess.run([out/'cases'],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},check=True)
