#!/usr/bin/env python3
"""Compare the worker's bytes and EnvironmentMetrics to both pinned ADS donor variants."""
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
driver_pin = '3d16518ab00f9f2f05473fc46930a312e72af685'
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-ads1x15-') as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for name in ('Adafruit_ADS1X15.cpp', 'Adafruit_ADS1X15.h'):
        (out/name).write_text(source(args.adafruit, driver_pin, name))
    # Keep all three upstream measurement methods verbatim. Substitute only
    # class ownership/declaration and the Arduino logging/clock environment.
    name = 'src/modules/Telemetry/Sensor/ADS1X15Sensor.'
    header = source(args.stock, stock_pin, name + 'h')
    measurements = header[header.index('struct _ADS1X15Measurement {'):header.rindex('#endif')]
    bodies = source(args.stock, stock_pin, name + 'cpp')
    bodies = bodies[bodies.index('struct _ADS1X15Measurement ADS1X15Sensor::'):bodies.rindex('#endif')]
    oracle = '''
#include <Adafruit_ADS1X15.h>
#define LOG_DEBUG(...)
namespace ScanI2C { enum class DeviceType { ADS1X15, ADS1X15_ALT }; }
template<class Model> struct OwnedAdc : Model { ~OwnedAdc() { delete this->m_i2c_dev; } };
'''
    for model in ('1015', '1115'):
        oracle += 'namespace oracle' + model + ' {\n' + measurements + '''
struct ADS1X15Sensor {
    OwnedAdc<Adafruit_ADS''' + model + '''> ads1x15;
    ScanI2C::DeviceType _deviceType{};
    _ADS1X15Measurement getMeasurement(uint8_t);
    _ADS1X15Measurements getMeasurements();
    bool getMetrics(meshtastic_Telemetry *);
};
''' + bodies + '\n}\n'
    (out/'oracle.h').write_text(oracle + '\n#undef LOG_DEBUG\n')
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_MT_ADS1X15=1', '-DCONFIG_MT_ACCESSORY_SENSORS=1',
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
        subprocess.run([*host.cxx(), '-std=c++17', *flags, here/'cases.cpp', out/'Adafruit_ADS1X15.cpp',
                        app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp', app/'src/Protobuf.cpp',
                        *objects, '-o', out/'ads-parity-test'], check=True)
        subprocess.run([out/'ads-parity-test'], check=True, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
