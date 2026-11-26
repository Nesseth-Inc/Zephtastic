#!/usr/bin/env python3
"""Compare the pinned TSL2591 and TSL2561 sensors with the stock environment telemetry worker."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
app = here.parents[1]
root = app.parent
sys.path.insert(0, str(root))
import host_test_toolchain as host

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
parser.add_argument('--tsl2591', type=Path, required=True)
parser.add_argument('--tsl2561', type=Path, required=True)
args = parser.parse_args()
stock_pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
driver2591 = 'a857bf93c10b5d60d02928c7a6c364e67ff5e0f3'
driver2561 = 'bf2adc60e22a46356d48755931852d2d6d0d2238'
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-tsl-light-') as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for cpp, repo, pin in [('Adafruit_TSL2591',args.tsl2591,driver2591),('Adafruit_TSL2561_U',args.tsl2561,driver2561)]:
        for ext in ('.h','.cpp'):
            (out/(cpp+ext)).write_text(source(repo,pin,cpp+ext))
    oracle='''
#include <Adafruit_TSL2591.h>
#include <Adafruit_TSL2561_U.h>
#define LOG_INFO(...)
namespace ScanI2C { struct FoundDevice {}; }
'''
    for model in ('2591','2561'):
        name = 'TSL'+model+'Sensor'
        driver = 'Adafruit_TSL2591 tsl;' if model=='2591' else 'Adafruit_TSL2561_Unified tsl{TSL2561_ADDR_LOW,12345};'
        oracle+='struct '+name+'{'+driver+'''
    bool status=false;
    void initI2CSensor(){}
    bool initDevice(TwoWire *,ScanI2C::FoundDevice *);
    bool getMetrics(meshtastic_Telemetry *);
};
'''
        donor=source(args.stock,stock_pin,'src/modules/Telemetry/Sensor/'+name+'.cpp')
        oracle+=donor[donor.index('bool '+name+'::initDevice'):donor.rindex('#endif')]
    (out/'oracle.h').write_text(oracle)
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_MT_NATIVE_LUX=1', '-DCONFIG_MT_NATIVE_TSL2591=1', '-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1', '-DCONFIG_MT_TSL_LIGHT=1', '-DCONFIG_MT_ACCESSORY_SENSORS=1',
                 *(['-DEKEYREJECTED=129'] if sys.platform == 'darwin' else [])]
        includes = (here/'mocks', app/'tests/stock_sensor_channels/mocks', out, app/'src',
                    app/'src/pki', app/'experimental/pki', vendor,
                    root/'zephcore/lib/monocypher', root/'zephcore/adapters/sensors', proto/'nanopb')
        flags.extend('-I' + str(i) for i in includes)
        objects = []
        for name in ('pb_common', 'pb_encode', 'pb_decode'):
            obj = out/(name + '.o')
            subprocess.run([*host.cc(), '-std=c11', *flags, '-c', proto/'nanopb'/(name+'.c'), '-o', obj], check=True)
            objects.append(obj)
        for mode in ([], ['-DTEST_TSL2561=1']):
            subprocess.run([*host.cxx(), '-std=c++17', *flags, *mode, here/'cases.cpp', out/'Adafruit_TSL2591.cpp', out/'Adafruit_TSL2561_U.cpp',
                            app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp', app/'src/Protobuf.cpp',
                            *objects, '-o', out/'tsl-parity-test'], check=True)
            subprocess.run([out/'tsl-parity-test'], check=True, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
