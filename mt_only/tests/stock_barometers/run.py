#!/usr/bin/env python3
"""Compile the four Meshtastic owners and their pinned Adafruit arithmetic against the production worker."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from native_compare import extract

here = Path(__file__).resolve().parent
app = here.parents[1]
root = app.parent
sys.path.insert(0, str(root))
import host_test_toolchain as host

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
parser.add_argument('--zephyr', type=Path, help='Also run the native SDK pressure differential')
for lib in ('aht', 'bmp085', 'bmp3', 'dps'):
    parser.add_argument('--'+lib, type=Path, required=True)
args = parser.parse_args()
pins = dict(stock='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c', aht='81dae1433eb0aaf8eafbadbc48669bad104d367c',
            bmp085='9d5057fb3e41afe0b4b7d97ceae1a97d53925c15', bmp3='82e3aa6407f299180a7b190eba5653ca221805a5',
            dps='5c28dbd2319b87e5a2a9b19c8e6147ef68fdb04a')

def source(repo, path):
    return subprocess.check_output(['git','-C',getattr(args,repo),'show',pins[repo]+':'+path],text=True)

def function(src, signature):
    start=src.index(signature)
    return src[start:src.index('\n}',start)+2]

def methods(repo, path, names):
    src=source(repo,path)
    return '\n'.join(function(src,sig) for sig in names)

def sensor(file, name):
    return methods('stock', 'src/modules/Telemetry/Sensor/'+file+'.cpp', ['bool '+name+'::getMetrics'])

proto = root/'zephcore/bench/upstream-differential/source'
vendor = root/'zephcore/adapters/bench/vendor'
with tempfile.TemporaryDirectory(prefix='zt-baro-') as temp:
    out=Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for name in ('bmp3.c','bmp3.h','bmp3_defs.h'):
        (out/name).write_text(source('bmp3',name))
    (out/'bosch.c').write_text('''#include "bmp3.c"
void bmp3_oracle(const uint8_t *c,const uint8_t *r,double *t,double *p) {
    struct bmp3_dev dev={0}; parse_calib_data(c,&dev);
    struct bmp3_uncomp_data raw={0}; parse_sensor_data(r,&raw);
    *t=compensate_temperature(&raw,&dev.calib_data);
    *p=compensate_pressure(&raw,&dev.calib_data);
}
''')
    oracle=(here/'oracle.cpp.in').read_text()
    blocks = dict(AHT_READ=methods('aht','Adafruit_AHTX0.cpp',['bool Adafruit_AHTX0::getEvent(']),
        AHT_METRICS=sensor('AHT10','AHT10Sensor'), BMP085_METRICS=sensor('BMP085Sensor','BMP085Sensor'),
        BMP085_FUNCS=methods('bmp085','Adafruit_BMP085.cpp',['int32_t Adafruit_BMP085::computeB5','float Adafruit_BMP085::readTemperature','int32_t Adafruit_BMP085::readPressure']),
        DPS_METRICS=sensor('DPS310Sensor','DPS310Sensor'),
        DPS_FUNCS=methods('dps','Adafruit_DPS310.cpp',['static int32_t twosComplement','void Adafruit_DPS310::_readCalibration','void Adafruit_DPS310::_read(','bool Adafruit_DPS310::getEvents']),
        BMP3_METRICS=sensor('BMP3XXSensor','BMP3XXSensor'))
    for name,body in blocks.items():
        oracle=oracle.replace('@'+name+'@',body)
    (out/'oracle.cpp').write_text(oracle)
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_STOCK_BAROMETERS=1',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else [])]
        inc=(here/'mocks',app/'tests/stock_ads1x15/mocks',app/'tests/stock_sensor_channels/mocks',out,
             app/'src',app/'src/pki',app/'experimental/pki',vendor,root/'zephcore/lib/monocypher',
             root/'zephcore/adapters/sensors',proto/'nanopb')
        flags.extend('-I'+str(i) for i in inc)
        objects=[]
        native=[extract(args.zephyr,out,function)] if args.zephyr else []
        if native:
            flags.append('-DTEST_NATIVE_COMPENSATION')
        for path in [*native,out/'bosch.c',*[proto/'nanopb'/(n+'.c') for n in ('pb_common','pb_encode','pb_decode')]]:
            obj=out/(path.name+'.o')
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',path,'-o',obj],check=True)
            objects.append(obj)
        # The stock BMP085 shifts a negative calibration value. Preserve the donor's target arithmetic
        # in its translation unit while keeping production code under the full undefined-behavior checks.
        obj=out/'oracle.o'
        subprocess.run([*host.cxx(),'-std=c++17',*flags,'-fno-sanitize=shift','-c',out/'oracle.cpp','-o',obj],check=True)
        for profile in ([],['-DBMP3_PROFILE=1','-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1']):
            binary=out/'barometers-test'
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*profile,here/'cases.cpp',
                            app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',app/'src/Protobuf.cpp',
                            obj,*objects,'-o',binary],check=True)
            subprocess.run([binary],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
