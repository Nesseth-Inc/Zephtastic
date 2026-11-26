#!/usr/bin/env python3
"""Compare the single stock SHT owner and Sensirion v1.2.6 with declared hardware fixtures."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from native_compare import extract
from owner_fixture import check_owner
here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock',type=Path,required=True)
parser.add_argument('--zephyr',type=Path,help='Also compile the pinned native SHT C channel conversions')
parser.add_argument('--sensirion',type=Path,required=True)
args=parser.parse_args()
stock='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
sensor='315c1c9c009d5da5557c52572f1de13ec6d8964b'
def get(repo,pin,path):
    return subprocess.check_output(['git','-C',repo,'show',pin+':'+path],text=True)
def func(src,sig):
    a=src.index(sig)
    return src[a:src.index('\n}',a)+2]
proto=root/'zephcore/bench/upstream-differential/source'
vendor=root/'zephcore/adapters/bench/vendor'
with tempfile.TemporaryDirectory(prefix='zt-sht-') as temp:
    out=Path(temp)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):
        (out/'meshtastic'/h.name).symlink_to(h)
    for name in ('SHTSensor.cpp','SHTSensor.h'):
        src=get(args.sensirion,sensor,name)
        if name.endswith('.cpp'):
            # The upstream buffer has one byte on SHT4x, although byte 1 is always assigned.
            # Only the wire length is variable. Keep the original command, timing and conversion.
            a=src.index('bool SHTI2cSensor::readSample()')
            old='uint8_t cmd[mCmd_Size];'
            assert src.count(old)==2
            src=src[:a]+src[a:].replace(old,'uint8_t cmd[2];',1)
        (out/name).write_text(src)
    scanner=get(args.stock,stock,'src/detect/ScanI2CTwoWire.cpp')
    owner=get(args.stock,stock,'src/modules/Telemetry/Sensor/SHTXXSensor.cpp')
    (out/'oracle.h').write_text('''
#include <Arduino.h>
#include <SHTSensor.h>
#define LOG_DEBUG(...)
#define LOG_INFO(...)
#define LOG_ERROR(...)
#define LOG_WARN(...)
namespace ScanI2C {struct FoundDevice {struct {unsigned address;} address;};}
struct SHTXXSensor {
    SHTSensor sht;
    TwoWire *_bus=nullptr;
    int _address=0,status=0;
    const char *sensorVariant=nullptr;
    SHTSensor::SHTAccuracy accuracy{};
    void initI2CSensor(){}
    bool initDevice(TwoWire *,ScanI2C::FoundDevice *);
    void getSensorVariant(SHTSensor::SHTSensorType);
    bool getMetrics(meshtastic_Telemetry *);
    bool setAccuracy(SHTSensor::SHTAccuracy);
};
''' + '\n'.join(func(owner,s) for s in ('void SHTXXSensor::getSensorVariant','bool SHTXXSensor::initDevice',
                'bool SHTXXSensor::getMetrics','bool SHTXXSensor::setAccuracy'))
                +'\n'+func(scanner,'static uint8_t crcSHT2X')+'\n'+func(scanner,'bool detectSHT21SerialNumber'))
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_STOCK_SHT=1','-DCONFIG_MT_SHTXX_ADMIN=1',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else [])]
        inc=(here/'mocks',app/'tests/stock_ads1x15/mocks',app/'tests/stock_sensor_channels/mocks',out,
             app/'src',app/'src/pki',app/'experimental/pki',vendor,root/'zephcore/lib/monocypher',
             root/'zephcore/adapters/sensors',proto/'nanopb')
        flags.extend('-I'+str(i) for i in inc)
        check_owner(host,flags,out,app,here)
        objs=[]
        if args.zephyr:
            flags.append('-DTEST_NATIVE_SHT')
            obj=out/'native.o'
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',extract(args.zephyr,out,func),'-o',obj],check=True)
            objs.append(obj)
        for n in ('pb_common','pb_encode','pb_decode'):
            obj=out/(n+'.o')
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',proto/'nanopb'/(n+'.c'),'-o',obj],check=True)
            objs.append(obj)
        binary=out/'sht'
        subprocess.run([*host.cxx(),'-std=c++17',*flags,here/'cases.cpp',out/'SHTSensor.cpp',
                        app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',app/'src/Protobuf.cpp',
                        *objs,'-o',binary],check=True)
        subprocess.run([binary],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
