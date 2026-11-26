#!/usr/bin/env python3
"""Compare optional particulate producers and phone wire with pinned stock source."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--stock', type=Path, required=True)
args.add_argument('--hm330x', type=Path, required=True)
args=args.parse_args()
stock='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
seeed='2704ca254c7e2136c52ac23198dd05f5ba1e2f04'
proto=root/'zephcore/bench/upstream-differential/source'
def read(repo,pin,path):
    return subprocess.check_output(['git','-C',repo,'show',pin+':'+path],text=True)
with tempfile.TemporaryDirectory(prefix='zt-particulate-') as directory:
    out=Path(directory)
    (out/'meshtastic').mkdir()
    for p in proto.glob('*.pb.h'):
        (out/'meshtastic'/p.name).symlink_to(p)
    for name in ('Seeed_HM330X.cpp','Seeed_HM330X.h','HM330XErrorCode.h','I2COperations.h','I2COperations.cpp'):
        (out/name).write_text(read(args.hm330x,seeed,'src/'+name))
    source=read(args.stock,stock,'src/modules/Telemetry/Sensor/HM330XSensor.cpp')
    start=source.index('bool HM330XSensor::getMetrics(')
    oracle='''#include <Seeed_HM330X.h>
#define LOG_ERROR(...)
#define LOG_WARN(...)
#define LOG_DEBUG(...)
struct HM330XSensor { HM330X hm330x; uint8_t buffer[30]{}; bool getMetrics(meshtastic_Telemetry *); };
'''+source[start:source.index('\n}',start)+2]
    pm=read(args.stock,stock,'src/modules/Telemetry/Sensor/PMSA003ISensor.cpp')
    oracle += """
#define LOG_INFO(...)
#define PMSA003I_ENABLE_PIN 7
#define PMSA003I_FRAME_LENGTH 32
#define PMSA003I_WARMUP_MS 30000
struct PMSA003ISensor {
  enum State { PMSA003I_IDLE, PMSA003I_ACTIVE };
  State state=PMSA003I_ACTIVE;
  const char *sensorName="pmsa";
  uint32_t pmMeasureStarted=0;
  uint16_t computedChecksum=0,receivedChecksum=0;
  TwoWire *_bus=&Wire;
  uint8_t _address=0x12,buffer[32]{};
  bool getMetrics(meshtastic_Telemetry *); bool isActive(); int32_t wakeUpTimeMs(); int32_t pendingForReadyMs();
  bool canSleep(); void sleep(); uint32_t wakeUp();
};
""" + pm[pm.index('bool PMSA003ISensor::getMetrics('):pm.rindex('#endif')]
    (out/'oracle.h').write_text(oracle+'\n#undef LOG_ERROR\n#undef LOG_WARN\n#undef LOG_DEBUG\n')
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_HM330X=1',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else [])]
        for p in (here/'mocks',app/'tests/stock_sensor_channels/mocks',out,app/'src',app/'src/pki',
                  app/'experimental/pki',root/'zephcore/adapters/bench/vendor',root/'zephcore/lib/monocypher',
                  root/'zephcore/adapters/sensors',proto/'nanopb'):
            flags.append('-I'+str(p))
        objs=[]
        for n in ('pb_common','pb_encode','pb_decode'):
            obj=out/(n+'.o')
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',proto/'nanopb'/(n+'.c'),'-o',obj],check=True)
            objs.append(obj)
        for mode in ([],['-DCONFIG_MT_PMSA003I=1']):
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*mode,here/'cases.cpp',out/'Seeed_HM330X.cpp',
                        out/'I2COperations.cpp',app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',
                        app/'src/Protobuf.cpp',app/'src/telemetry/CompletionTelemetry.cpp',
                        *objs,'-o',out/'particle-parity'],check=True)
            subprocess.run([out/'particle-parity'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
