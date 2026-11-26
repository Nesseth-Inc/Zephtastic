#!/usr/bin/env python3
"""Exercise the real accessory worker against two Meshtastic handlers and their pinned drivers."""
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
for name in ('stock','sfa','rak','core'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
pins = dict(stock='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c', sfa='bc5be3d349f8fea453ab45da767bdbf51c1766b7',
            rak='3fbe21cd82bcf83d3057bb0e92ac1963f2edcb13',core='6c087aae9e11c66e2b01a0c79d9f1632e1821b65')
def git(key, *cmd):
    return subprocess.check_output(['git', '-C', getattr(args,key), *cmd],text=True)
def source(key, p):
    return git(key,'show',pins[key]+':'+p)

proto = root/'zephcore/bench/upstream-differential/source'
with tempfile.TemporaryDirectory(prefix='zt-air-soil-') as directory:
    out = Path(directory)
    (out/'meshtastic').mkdir()
    for p in proto.glob('*.pb.h'):
        (out/'meshtastic'/p.name).symlink_to(p)
    driver_sources = []
    for key in ('core','sfa','rak'):
        paths = git(key,'ls-tree','-r','--name-only',pins[key]).splitlines()
        for p in paths:
            name = Path(p).name
            if not (p.startswith('src/') or (key=='rak' and p.startswith('RAK12035_SoilMoisture.'))):
                continue
            if p.endswith('.cpp') and name.startswith('SensirionShdlc'):
                continue
            if p.endswith(('.h','.cpp')):
                (out/name).write_text(source(key,p))
                if p.endswith('.cpp'):
                    driver_sources.append(out/name)
    oracle = '''#include <RAK12035_SoilMoisture.h>
#include <SensirionI2cSfa3x.h>
#define LOG_DEBUG(...)
#define LOG_ERROR(...)
#define LOG_WARN(...)
#define RESTORE_3V3_POWER()
struct SFA30Sensor {
    SensirionI2cSfa3x sfa30;
    bool isError(uint16_t e) { return e != 0; }
    bool getMetrics(meshtastic_Telemetry *);
};
struct RAK12035Sensor {
    RAK12035 sensor;
    bool getMetrics(meshtastic_Telemetry *);
};
'''
    for name in ('SFA30Sensor','RAK12035Sensor'):
        body = source('stock','src/modules/Telemetry/Sensor/'+name+'.cpp')
        start = body.index('bool '+name+'::getMetrics(')
        oracle += body[start:body.index('\n}',start)+2]+'\n'
    (out/'oracle.h').write_text(oracle+'\n#undef LOG_DEBUG\n#undef LOG_ERROR\n#undef LOG_WARN\n')
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_RAK12035=1',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else [])]
        for p in (here/'mocks',app/'tests/stock_sensor_channels/mocks',out,app/'src',app/'src/pki',
                  app/'experimental/pki',root/'zephcore/adapters/bench/vendor',root/'zephcore/lib/monocypher',
                  root/'zephcore/adapters/sensors',proto/'nanopb'):
            flags.append('-I'+str(p))
        objects=[]
        for name in ('pb_common','pb_encode','pb_decode'):
            obj=out/(name+'.o')
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',proto/'nanopb'/(name+'.c'),'-o',obj],check=True)
            objects.append(obj)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,here/'cases.cpp',*driver_sources,
                        app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',app/'src/Protobuf.cpp',
                        app/'src/telemetry/CompletionTelemetry.cpp',*objects,'-o',out/'air-soil-parity'], check=True)
        subprocess.run([out/'air-soil-parity'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
