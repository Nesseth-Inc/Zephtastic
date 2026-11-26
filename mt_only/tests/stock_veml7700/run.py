#!/usr/bin/env python3
"""Compare optical measurements, exposure timing and phone fields with both pinned VEML7700 sources."""
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
parser.add_argument('--adafruit', type=Path, required=True)
args = parser.parse_args()
stock_pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
driver_pin = '0dc00faa4313e6074a00deeb354f7099b8dd0805'
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-veml7700-') as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for name in ('Adafruit_VEML7700.cpp', 'Adafruit_VEML7700.h'):
        (out/name).write_text(source(args.adafruit, driver_pin, name))
    path = 'src/modules/Telemetry/Sensor/VEML7700Sensor.cpp'
    donor = source(args.stock, stock_pin, path)
    oracle = '''
#include <Adafruit_VEML7700.h>
#define LOG_INFO(...)
namespace ScanI2C { struct FoundDevice {}; }
struct VEML7700Sensor {
    const float MAX_RES = 0.0036, GAIN_MAX = 2, IT_MAX = 800;
    Adafruit_VEML7700 veml7700;
    bool status = false;
    void initI2CSensor() {}
    bool initDevice(TwoWire *, ScanI2C::FoundDevice *);
    float computeLux(uint16_t, bool);
    float getResolution();
    bool getMetrics(meshtastic_Telemetry *);
};
'''
    (out/'oracle.h').write_text(oracle + donor[donor.index('bool VEML7700Sensor::initDevice'):donor.rindex('#endif')] + '\n#undef LOG_INFO\n')
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_MT_VEML7700=1', '-DCONFIG_MT_ACCESSORY_SENSORS=1',
                 *(['-DEKEYREJECTED=129'] if sys.platform == 'darwin' else [])]
        includes = (here/'mocks', app/'tests/stock_ads1x15/mocks', app/'tests/stock_sensor_channels/mocks', out, app/'src',
                    app/'src/pki', app/'experimental/pki', vendor,
                    root/'zephcore/lib/monocypher', root/'zephcore/adapters/sensors', proto/'nanopb')
        flags.extend('-I' + str(i) for i in includes)
        objects = []
        for name in ('pb_common', 'pb_encode', 'pb_decode'):
            obj = out/(name + '.o')
            subprocess.run([*host.cc(), '-std=c11', *flags, '-c', proto/'nanopb'/(name+'.c'), '-o', obj], check=True)
            objects.append(obj)
        for mode in ([], ['-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1']):
            subprocess.run([*host.cxx(), '-std=c++17', *flags, *mode, here/'cases.cpp', out/'Adafruit_VEML7700.cpp',
                            app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp', app/'src/Protobuf.cpp',
                            *objects, '-o', out/'veml-parity-test'], check=True)
            subprocess.run([out/'veml-parity-test'], check=True, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
