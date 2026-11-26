#!/usr/bin/env python3
"""Compile the actual aggregate path and receiver with synthetic ESP/BT drivers."""
import argparse, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for key in ('stock','libpax','output'):p.add_argument('--'+key,type=Path,required=True)
a=p.parse_args();out=a.output.resolve();here=Path(__file__).resolve().parent;app=here.parents[1]
assert not out.exists() and not out.is_relative_to(app.parent)
out.mkdir(parents=True)
z=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').symlink_to(z,target_is_directory=True)
def donor(repo,oid,file):
    return subprocess.check_output(['git','-C',repo,'show',oid+':'+file],text=True)
def section(s,marker):
    pos=s.index(marker);first=s.index('{',pos);depth=0
    for i,c in enumerate(s[first:],first):
        depth+=(c=='{')-(c=='}')
        if not depth:return s[pos:i+1]
    raise ValueError(marker)
s=donor(a.stock,'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','src/modules/esp32/PaxcounterModule.h')
(out/'oracle.h').write_text(section(s,'bool isActive()')+'\n')
pax=donor(a.libpax,'6f52ee989301cdabaeef00bcbf93bff55708ce2f','lib/libpax/libpax.cpp')
(out/'oracle.h').open('a').write(section(pax,'IRAM_ATTR int mac_add(')+'\n')
for file in ('zephyr/kernel.h','zephyr/device.h','zephyr/devicetree.h','zephyr/drivers/pwm.h','zephyr/bluetooth/bluetooth.h','zephyr/bluetooth/conn.h','esp_err.h','esp_wifi.h'):
    target=out/file;target.parent.mkdir(exist_ok=True,parents=True);target.write_text('#include "fake.h"\n')
for abi,flags in [('normal',[]),('short',['-fshort-enums'])]:
    exe=out/abi
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*flags,
        '-DCONFIG_MT_PAXCOUNTER=1','-DCONFIG_MT_PAXCOUNTER_WIFI_ESP32=1','-DCONFIG_BT_OBSERVER=1',
        '-I'+str(out),'-I'+str(here),'-I'+str(app/'src'),'-I'+str(z/'nanopb'),str(here/'cases.cpp'),
        str(app/'src/modules/peripherals/ZephyrPeripherals.cpp'),'-o',str(exe)],check=True)
    subprocess.run([exe],check=True)
