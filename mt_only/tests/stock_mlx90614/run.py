#!/usr/bin/env python3
"""Compare infrared object-temperature and health messages with the pinned stock implementation."""
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
driver_pin = '8c32c95768e0960d7c27258b1d9bac16b8d7fdfc'
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-mlx90614-') as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for name in ('Adafruit_MLX90614.cpp', 'Adafruit_MLX90614.h'):
        (out/name).write_text(source(args.adafruit, driver_pin, name))
    path = 'src/modules/Telemetry/Sensor/MLX90614Sensor.cpp'
    donor = source(args.stock, stock_pin, path)
    module = source(args.stock, stock_pin, 'src/modules/Telemetry/HealthTelemetry.cpp')
    start = module.index('bool HealthTelemetryModule::getHealthTelemetry(')
    health = module[start:module.index('\n}', start)+2]
    oracle = '''
#include <Adafruit_MLX90614.h>
#define LOG_INFO(...)
#define LOG_DEBUG(...)
#define LOG_ERROR(...)
#define MLX90614_EMISSIVITY 0.98
constexpr int DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS=1000;
struct Device { unsigned first=0x5a; TwoWire *second=&Wire; } nodeTelemetrySensorsMap[1];
struct MLX90614Sensor {
    bool status=false;
    int sensorType=0;
    Adafruit_MLX90614 mlx;
    bool hasSensor(){return true;}
    int initI2CSensor(){return status?0:1;}
    void setup(); int32_t runOnce(); bool getMetrics(meshtastic_Telemetry *);
} mlx90614Sensor;
struct Optical {
    bool present=false;
    bool hasSensor() { return present; }
    bool getMetrics(meshtastic_Telemetry *m) {
        auto &h=m->variant.health_metrics;
        h.has_heart_bpm=h.has_spO2=true;h.heart_bpm=71;h.spO2=96;return true;
    }
} max30102Sensor;
int getTime() { return 17; }
struct HealthTelemetryModule { bool getHealthTelemetry(meshtastic_Telemetry *); };
'''
    (out/'oracle.h').write_text(oracle + donor[donor.index('int32_t MLX90614Sensor::runOnce'):donor.rindex('#endif')] + health)
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_MT_MLX90614=1', '-DCONFIG_MT_ACCESSORY_SENSORS=1',
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
        subprocess.run([*host.cxx(), '-std=c++17', *flags, here/'cases.cpp', out/'Adafruit_MLX90614.cpp',
                        app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp', app/'src/telemetry/CompletionTelemetry.cpp',
                        app/'src/Protobuf.cpp', *objects, '-o', out/'mlx-parity-test'], check=True)
        subprocess.run([out/'mlx-parity-test'], check=True, env={**os.environ, 'UBSAN_OPTIONS':'halt_on_error=1'})
