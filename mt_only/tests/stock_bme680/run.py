#!/usr/bin/env python3
"""Compile unmodified pinned upstream IAQ tests and ZT's cadence/metrics adapter."""
from pathlib import Path
import argparse, os, subprocess, sys, tempfile
here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
p=argparse.ArgumentParser()
p.add_argument('--stock',required=True,type=Path)
args=p.parse_args()
pin='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
def donor(path):return subprocess.check_output(['git','-C',args.stock,'show',pin+':'+path])
base='src/modules/Telemetry/Sensor/'
source=app/'src/stock/telemetry/bme680'
assert (source/'BME680IaqEstimator.h').read_bytes()==donor(base+'BME680IaqEstimator.h')
import re
ported=(source/'BME680IaqEstimator.cpp').read_bytes()
# Only the STL clamp spelling changes on the freestanding target.
normalized=re.sub(rb'#if defined\(CONFIG_MINIMAL_LIBCPP\).*?#endif',b'#include <algorithm>',ported,count=1,flags=re.S)
assert normalized.replace(b'bmeClamp(',b'std::clamp(')==donor(base+'BME680IaqEstimator.cpp')
inventory=(app/'docs/UPSTREAM-TELEMETRY-SENSORS.md').read_text()
listed={word.strip() for line in inventory.splitlines() if line.startswith('|') for word in line.split('|')[1].split(',')}
files=subprocess.check_output(['git','-C',args.stock,'ls-tree','-r','--name-only',pin,'src/modules/Telemetry/Sensor'],text=True).splitlines()
sensors={Path(f).stem for f in files if f.endswith('.cpp')}
assert len(sensors)==46 and sensors<=listed
test=donor('test/test_bme680_iaq/test_main.cpp')
unity=r'''#include <cassert>
#include <cmath>
#include <cstdlib>
#define TEST_ASSERT_TRUE(x,...) assert(x)
#define TEST_ASSERT_TRUE_MESSAGE(x,...) assert(x)
#define TEST_ASSERT_FALSE(x) assert(!(x))
#define TEST_ASSERT_EQUAL_INT(x,y) assert((x)==(y))
#define TEST_ASSERT_EQUAL_UINT8 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT16 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT32 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_MESSAGE(x,y,...) assert((x)==(y))
#define TEST_ASSERT_GREATER_THAN_UINT(x,y) assert((y)>(x))
#define TEST_ASSERT_UINT_WITHIN(x,y,z) assert(std::abs(double(y)-double(z))<=x)
#define TEST_ASSERT_UINT_WITHIN_MESSAGE(x,y,z,...) TEST_ASSERT_UINT_WITHIN(x,y,z)
#define UNITY_BEGIN() do{}while(0)
#define UNITY_END() 0
#define RUN_TEST(f) f()
'''
with tempfile.TemporaryDirectory(prefix='zt-bme680-') as folder:
    tmp=Path(folder)
    (tmp/'MeshTypes.h').write_text('');(tmp/'TestUtil.h').write_text('inline void initializeTestEnvironment(){}\n')
    (tmp/'unity.h').write_text(unity)
    portable=tmp/'modules/Telemetry/Sensor';portable.mkdir(parents=True)
    (portable/'BME680IaqEstimator.h').symlink_to(source/'BME680IaqEstimator.h')
    (tmp/'oracle.cpp').write_bytes(test+b'\nint main(){setup();}\n')
    proto=root/'zephcore/bench/upstream-differential/source'
    (tmp/'meshtastic').symlink_to(proto)
    env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'}
    for abi in ([],['-fshort-enums']):
        flags=[*host.cxx(),'-std=c++17','-g','-O1','-fsanitize=address,undefined',*abi,'-I'+str(tmp),
               '-I'+str(app/'src'),'-I'+str(proto/'nanopb'),'-DCONFIG_MT_BME680_IAQ=1','-DCONFIG_MINIMAL_LIBCPP=1']
        for case in (tmp/'oracle.cpp',here/'cases.cpp',here/'owner.cpp'):
            subprocess.run([*flags,'-I'+str(here/'mocks'),case,source/'BME680IaqEstimator.cpp','-o',tmp/'test'],check=True)
            subprocess.run([tmp/'test'],check=True,env=env)
        # Exercise the native Companion (USB/BLE), public RF responder and private endpoint.
        # The same BME fields must survive with and without generic environment channels.
        vendor=root/'zephcore/adapters/bench/vendor'
        mono=root/'zephcore/lib/monocypher'
        transport_flags=[*flags,'-I'+str(vendor),'-I'+str(mono),'-I'+str(app/'tests'),
                         *host.crypto_compile_flags(),*(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else [])]
        objects=[]
        for item in [mono/'monocypher.c',mono/'monocypher-ed25519.c',
                     *(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
            obj=tmp/(item.stem+'.o')
            subprocess.run([*host.cc(),'-std=c11',*abi,'-O1','-fsanitize=address,undefined','-c',item,'-o',obj],check=True)
            objects.append(obj)
        for generic in ([],['-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1']):
            subprocess.run([*transport_flags,*generic,app/'tests/environment_transport_tests.cpp',
                            app/'src/Protobuf.cpp',
                            vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objects,
                            *host.crypto_link_flags(),'-o',tmp/'transport'],check=True)
            subprocess.run([tmp/'transport'],check=True,env=env)
        print(f"PASS {test.count(b'RUN_TEST(')} pinned BME680 IAQ cases, ZT warm-up reboots, timing, gasless carry, zero-valued IAQ and metrics")
