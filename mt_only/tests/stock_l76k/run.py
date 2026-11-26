#!/usr/bin/env python3
"""Compare the passive XIAO L76K adapter with the pinned Meshtastic standby method."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
test = Path(__file__).resolve().parent
app = test.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
args = parser.parse_args()


def donor(path):
    return subprocess.check_output(['git', '-C', args.stock, 'show', f'{PIN}:{path}'], text=True)


def define(source, name):
    return re.search(r'^#define ' + name + r'\s+(\S+)', source, re.M).group(1)


variant = donor('variants/esp32s3/seeed_xiao_s3/variant.h')
assert [define(variant, k) for k in ('GPS_RX_PIN','GPS_TX_PIN','PIN_GPS_STANDBY')] == ['44','43','1']
assert define(donor('src/configuration.h'), 'GPS_BAUDRATE') == '9600'
polarity = define(donor('src/gps/GPS.h'), 'GPS_STANDBY_ACTIVE')
src = donor('src/gps/GPS.cpp')
start = src.index('void GPS::writePinStandby(bool standby)')
method = src[start:src.index('\n}',start)+2]
overlay = (app/'boards/xiao-l76k.overlay').read_text()
assert 'current-speed = <9600>' in overlay and 'gnss-standby-gpios = <&gpio0 1 GPIO_ACTIVE_LOW>' in overlay
assert 'reset-gpios' not in overlay and 'gnss-nmea-generic' in overlay
with tempfile.TemporaryDirectory(prefix='zt-l76k-') as tmp:
    build = Path(tmp)
    (build/'pinned.inc').write_text(f'#define GPS_STANDBY_ACTIVE {polarity}\n#define PIN_GPS_STANDBY 1\n'+method)
    common = re.search(r'serialSpeeds\[3\] = (\{[^}]+\})',src).group(1)
    rare = re.search(r'rareSerialSpeeds\[3\] = (\{[^}]+\})',src).group(1)
    repetitions = int(define(src,'GPS_PROBETRIES'))
    version = re.search(r'PROBE_SIMPLE\("L76K", "([^"]+)", "([^"]+)", GNSS_MODEL_MTK, (\d+)\)',src)
    hardware = re.search(r'PROBE_FAMILY\("ATGM33xx Family", "([^"]+)", atgm, (\d+)\)',src)
    assert version[2]=='$GPTXT,01,01,02,SW=' and version[3]==hardware[2]=='500'
    start=src.index('if (gnssModel == GNSS_MODEL_MTK) {')
    body=src[src.index('{',start)+1:src.index('} else if (gnssModel == GNSS_MODEL_MTK_L76B)',start)]
    (build/'setup-oracle.inc').write_text(
        f'constexpr const char *hardware="{hardware[1]}\\r\\n", *software="{version[1]}\\r\\n";\n'
        f'void configure(){{auto *_serial_gps=&serial;{body}}}\n'
        f'std::vector<uint32_t> rates(){{constexpr int GPS_BAUDRATE=9600; '
        f'const unsigned common[]={common}; const unsigned rare[]={rare}; std::vector<uint32_t> out; '
        f'for(int i=0;i<{repetitions};++i)for(auto r:common)out.push_back(r); '
        'for(auto r:rare)out.push_back(r);return out;}')
    for flags in ([], ['-fshort-enums']):
        for name in ('cases','setup'):
            exe = build/name
            subprocess.run([*shlex.split(os.getenv('CXX','c++')),'-std=c++17','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined',*flags,'-I'+str(build),'-I'+str(app/'src'),
                            test/(name+'.cpp'),'-o',exe],check=True)
            subprocess.run([exe],check=True)
