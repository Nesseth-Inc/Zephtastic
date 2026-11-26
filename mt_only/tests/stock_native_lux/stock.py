#!/usr/bin/env python3
"""Compare the continuous light worker with original Meshtastic and pinned sensor libraries."""
import argparse
import hashlib
import tarfile
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
parser.add_argument('--bh1750', type=Path)
parser.add_argument('--opt3001-archive', type=Path)
args = parser.parse_args()
if not args.bh1750 and not args.opt3001_archive:
    parser.error("supply --bh1750 or --opt3001-archive")
stock_pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
bh_pin = 'fcd30fea797383e5c29672549a65f97754fcd099'
proto = root / 'zephcore/bench/upstream-differential/source'
vendor = root / 'zephcore/adapters/bench/vendor'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-continuous-lux-') as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    cases = []
    if args.bh1750:
        for ext in ('.h','.cpp'):
            name='BH1750_WE'+ext
            (out/name).write_text(source(args.bh1750,bh_pin,'src/'+name))
        donor=source(args.stock,stock_pin,'src/modules/Telemetry/Sensor/BH1750Sensor.cpp')
        decl='''
    #include <BH1750_WE.h>
    #define LOG_INFO(...)
    #define BH1750_SENSOR_MODE BH1750Mode::CHM
    namespace ScanI2C { struct FoundDevice { struct {uint8_t address=0x23;} address; }; }
    struct BH1750Sensor{BH1750_WE bh1750;bool status=false;void initI2CSensor(){}
     bool initDevice(TwoWire*,ScanI2C::FoundDevice*);bool getMetrics(meshtastic_Telemetry*);};
    '''
        (out/'bh-oracle.h').write_text(decl+donor[donor.index('bool BH1750Sensor::initDevice'):donor.rindex('#endif')])
        cases.append(('stock.cpp','BH1750_WE.cpp',['-DCONFIG_MT_BH1750=1']))
    if args.opt3001_archive:
        assert hashlib.sha256(args.opt3001_archive.read_bytes()).hexdigest() == '95d3878e32ead6be60ade8636e0de8758244f1386f7d643e9130e54b1f73d8b4'
        with tarfile.open(args.opt3001_archive) as archive:
            for ext in ('.h','.cpp'):
                name='ClosedCube_OPT3001'+ext
                (out/name).write_bytes(archive.extractfile('src/'+name).read())
        donor=source(args.stock,stock_pin,'src/modules/Telemetry/Sensor/OPT3001Sensor.cpp')
        decl='''
#include <Wire.h>
#include <ClosedCube_OPT3001.h>
#define LOG_INFO(...)
#define LOG_ERROR(...)
namespace ScanI2C { struct FoundDevice { struct {uint8_t address=0x45;} address; }; }
struct OPT3001Sensor{ClosedCube_OPT3001 opt3001;bool status=false;void initI2CSensor(){}
 bool initDevice(TwoWire*,ScanI2C::FoundDevice*);bool getMetrics(meshtastic_Telemetry*);};
'''
        (out/'opt-oracle.h').write_text(decl+donor[donor.index('bool OPT3001Sensor::initDevice'):donor.rindex('#endif')])
        env=source(args.stock,stock_pin,'src/modules/Telemetry/EnvironmentTelemetry.cpp')
        donors=('LTR390UVSensor','VEML7700Sensor','TSL2591Sensor','OPT3001Sensor','TSL2561Sensor','BH1750Sensor')
        assert [env.index('addSensor<'+name+'>') for name in donors] == sorted(env.index('addSensor<'+name+'>') for name in donors)
        cases.append(('opt.cpp','ClosedCube_OPT3001.cpp',['-DCONFIG_MT_OPT3001=1','-DCONFIG_MT_TSL_LIGHT=1','-DCONFIG_MT_BH1750=1']))
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DARDUINO=10800', '-DCONFIG_MT_ACCESSORY_SENSORS=1',
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
        for fixture, library, macros in cases:
            subprocess.run([*host.cxx(), '-std=c++17', *flags, *macros, here/fixture,out/library,
                        app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',app/'src/Protobuf.cpp',
                        *objects, '-o',out/'stock-lux'],check=True)
            subprocess.run([out/'stock-lux'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        if args.opt3001_archive:
            # Native alias order must not affect any valid later stock donor.
            # Model IDs match the verified registration order, starting with LTR390 at 1.
            mixed = ['-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1','-DCONFIG_MT_NATIVE_LUX=1',
                     '-DCONFIG_MT_TSL_LIGHT=1','-DCONFIG_MT_OPT3001=1','-DCONFIG_MT_BH1750=1','-DCONFIG_MT_VEML7700=1']
            for first,second in ((6,5),(4,3),(4,5),(4,2)):
                subprocess.run([*host.cxx(),'-std=c++17','-I'+str(here/'mixed_mocks'),*flags,*mixed,
                                f'-DNATIVE_FIRST={first}',f'-DNATIVE_SECOND={second}',here/'mixed.cpp',
                                app/'src/Protobuf.cpp',*objects,'-o',out/'mixed-lux'],check=True)
                subprocess.run([out/'mixed-lux'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
