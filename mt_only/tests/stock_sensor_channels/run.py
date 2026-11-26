#!/usr/bin/env python3
"""Exercise actual Zephyr sensor producers, Admin dispatch and worker using deterministic I/O."""
import argparse, os, re, subprocess, sys, tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock',type=Path,required=True)
parser.add_argument('--maxim',type=Path,help='SparkFun MAX3010x Git checkout for the independent v1.1.2 optical oracle')
args=parser.parse_args()
pin='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
proto=root/'zephcore/bench/upstream-differential/source'
vendor=root/'zephcore/adapters/bench/vendor'
mono=root/'zephcore/lib/monocypher'
with tempfile.TemporaryDirectory(prefix='zt-sensor-') as directory:
    out=Path(directory)
    (out/'meshtastic').mkdir()
    for h in proto.glob('*.pb.h'):(out/'meshtastic'/h.name).symlink_to(h)
    (out/'CO2Sensor.h').write_bytes(subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:src/modules/Telemetry/Sensor/CO2Sensor.h']))
    # Compile the pinned state-validity methods with only their clock and storage members.
    sensor_path='src/modules/Telemetry/Sensor/SENXXSensor'
    def donor(suffix):
        return subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:{sensor_path}.{suffix}'],text=True)
    sen_h,sen_cpp=donor('h'),donor('cpp')
    constants=('SENXX_VOC_VALID_TIME','SENXX_VOC_VALID_DATE','SENXX_VOC_STATE_WARMUP_S',
               'SENXX_VOC_STATE_BUFFER_SIZE','SENXX_CLEANING_DURATION_MS','ONE_WEEK_IN_SECONDS','SEN6X_CO2_WARMUP_MS')
    pieces=[re.search(r'^#define '+name+r' +[0-9]+$',sen_h,re.M).group() for name in constants]
    pieces+=['namespace stockSensorOracle {',
             'struct SENXXSensor { uint32_t nowMs=0, rhtGasMeasureStarted=0, vocTime=0;',
             'uint8_t vocState[SENXX_VOC_STATE_BUFFER_SIZE]{}; uint32_t millis() const{return nowMs;}',
             'bool vocStateRecent(uint32_t); bool vocStateStable(); bool vocStateValid(); };']
    for method in ('vocStateRecent','vocStateStable','vocStateValid'):
        start=sen_cpp.index('bool SENXXSensor::'+method+'(')
        end=sen_cpp.index('\n}',start)+2
        pieces.append(sen_cpp[start:end])
    (out/'sen_maintenance_oracle.h').write_text('\n'.join([*pieces,'}\n']))
    # Compile the upstream conversion bodies with the same three bus bytes.
    sensor_dir='src/modules/Telemetry/Sensor/'
    range_oracle = [
        '#pragma once', '#include <stdint.h>', '#define LOG_DEBUG(...)',
        'namespace donorRange {',
        'struct Wire { uint8_t data[3]{}; unsigned i=0; void beginTransmission(uint8_t){}',
        'void write(uint8_t){} int endTransmission(){return 0;}',
        'int requestFrom(uint8_t,uint8_t){i=0;return 3;} int read(){return data[i++];} int available(){return 3;} };',
        'inline void delay(unsigned){}',
    ]
    for name, method in [('RCWL9620Sensor','getDistance'),('CGRadSensSensor','getStaticRadiation')]:
        source=subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:{sensor_dir}{name}.cpp'],text=True)
        start=source.index('float '+name+'::'+method+'()')
        end=source.index('\n}',start)+2
        range_oracle.extend([f'struct {name} {{ Wire *_wire; uint8_t _addr=0; float {method}(); }};',source[start:end]])
    (out/'range_oracle.h').write_text('\n'.join([*range_oracle,'}', '#undef LOG_DEBUG']))
    source=subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:{sensor_dir}LTR390UVSensor.cpp'],text=True)
    start=source.index('bool LTR390UVSensor::getMetrics(')
    end=source.index('\n}',start)+2
    (out/'light_oracle.h').write_text(
        '#pragma once\n#define LOG_DEBUG(...)\nnamespace donorLight {\n'
        'enum { LTR390_MODE_ALS=0,LTR390_MODE_UVS=1,LTR390_GAIN_18=4,LTR390_GAIN_1=0 };\n'
        'struct Light { bool ready=true;unsigned mode=LTR390_MODE_UVS,gain=4,als=0,uv=0;'
        'bool newDataAvailable(){return ready;} unsigned getMode(){return mode;} void setMode(unsigned v){mode=v;}'
        'void setGain(unsigned v){gain=v;}unsigned readALS(){return als;}unsigned readUVS(){return uv;} };\n'
        'struct LTR390UVSensor { Light ltr390uv;float lastUVReading=0,lastLuxReading=0;'
        'bool getMetrics(meshtastic_Telemetry *); };\n'+source[start:end]+'\n}\n#undef LOG_DEBUG\n')
    def method(path,signature):
        source=subprocess.check_output(['git','-C',args.stock,'show',f'{pin}:{path}'],text=True)
        start=source.index(signature);return source[start:source.index('\n}',start)+2]
    weather_oracle=['#pragma once','#include <string>','#define LOG_INFO(...)','namespace donorWeather {',
        'struct GeoCoord{ static unsigned int bearingToDegrees(const char *); };',
        method('src/gps/GeoCoord.cpp','unsigned int GeoCoord::bearingToDegrees('),
        method('src/meshUtils.cpp','float parseDecimalFloat('),
        'struct Station{std::string getValue(const char *key){if(!strcmp(key,"Temp"))return " -7.75";'
        'if(!strcmp(key,"Humi"))return "81.25";if(!strcmp(key,"Dir"))return "SSE";'
        'if(!strcmp(key,"Speed"))return "7.25";return "1003.5";}};',
        'struct DFRobotLarkSensor{Station lark;bool getMetrics(meshtastic_Telemetry *);};',
        method(sensor_dir+'DFRobotLarkSensor.cpp','bool DFRobotLarkSensor::getMetrics('),'}','#undef LOG_INFO']
    (out/'weather_oracle.h').write_text('\n'.join(weather_oracle))
    (out/'MeshModule.h').write_text('#pragma once\n')
    oracle=[]
    if args.maxim:
        maxim_pin='72d5308df500ae1a64cc9d63e950c68c96dc78d5'
        for filename in ('spo2_algorithm.cpp','spo2_algorithm.h'):
            (out/filename).write_bytes(subprocess.check_output(['git','-C',args.maxim,'show',f'{maxim_pin}:src/{filename}']))
        (out/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <algorithm>\nusing std::min;\n')
        oracle=[out/'spo2_algorithm.cpp','-DMT_MAXIM_ORACLE=1']
    driver_headers=out/'scd4x-host'
    for h in ('device.h','kernel.h','drivers/sensor.h','drivers/i2c.h','logging/log.h',
              'drivers/gpio.h','pm/device.h','drivers/sensor/sht4x.h','sys/__assert.h','sys/byteorder.h','sys/crc.h','devicetree.h'):
        dest=driver_headers/'zephyr'/h
        dest.parent.mkdir(exist_ok=True,parents=True)
        dest.write_text('#include "scd4x_platform.h"\n')
    overlay=app/'dependency-overlay/zephyr'
    for abi in ([],['-fshort-enums']):
        flags=[*abi,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-DCONFIG_MT_SHTXX_ADMIN=1','-DCONFIG_MT_MAX30102_OPTICAL=1','-DCONFIG_MT_ONE_WIRE_TELEMETRY=1','-DCONFIG_MT_ENVIRONMENT_SENSORS=1','-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1','-DCONFIG_MT_SCD4X_ADMIN=1',
               *(['-DEKEYREJECTED=129'] if sys.platform=='darwin' else []),*host.crypto_compile_flags(),
               *['-I'+str(i) for i in (here/'mocks',out,app/'src',app/'tests',app/'src/pki',app/'experimental/pki',vendor,mono,root/'zephcore/adapters/sensors',proto/'nanopb')]]
        subprocess.run([*host.cc(),'-std=c11',*abi,'-O1','-g','-fsanitize=address,undefined',
                        '-I'+str(here),'-I'+str(driver_headers),'-I'+str(overlay/'include'),
                        '-I'+str(overlay/'drivers/sensor/sensirion/scd4x'),here/'scd4x_driver.c',
                        '-o',out/'driver'],check=True)
        subprocess.run([out/'driver'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        for chip,mode in [('sht3xd','SINGLE_SHOT'),('sht3xd','PERIODIC'),('sht4x','SINGLE_SHOT')]:
            subprocess.run([*host.cc(),'-std=c11',*abi,'-O1','-g','-fsanitize=address,undefined',
                            '-DCONFIG_MT_SHTXX_ADMIN=1', '-DTEST_'+chip.upper(),
                            '-DCONFIG_SHT3XD_'+mode+'_MODE=1','-DCONFIG_SHT3XD_REPEATABILITY_HIGH=1',
                            '-DCONFIG_SHT3XD_MPS_1=1','-I'+str(here),'-I'+str(driver_headers),
                            '-I'+str(overlay/'drivers/sensor/sensirion'/chip),here/'sht_driver.c',
                            '-o',out/'sht'],check=True)
            subprocess.run([out/'sht'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        objs=[]
        for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*[proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c')]]:
            obj=out/(src.stem+'.o');objs.append(obj)
            subprocess.run([*host.cc(),'-std=c11',*flags,'-c',src,'-o',obj],check=True)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,here/'cases.cpp',here/'admin.cpp',here/'optical.cpp',*oracle,app/'src/Protobuf.cpp',
            app/'src/stock/telemetry/ZephyrScd4xAdmin.cpp',app/'src/stock/telemetry/ZephyrOneWire.cpp',
            app/'src/stock/telemetry/max30102/ZephyrOptical.cpp',app/'src/stock/telemetry/max30102/MaximAlgorithm.cpp',app/'src/telemetry/CompletionTelemetry.cpp',
            vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objs,*host.crypto_link_flags(),'-o',out/'cases'],check=True)
        subprocess.run([out/'cases'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        accessories_flags=[f for f in flags if not f.startswith('-DCONFIG_MT_')]
        subprocess.run([*host.cxx(),'-std=c++17',*accessories_flags,'-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_LARK_WEATHER=1','-DCONFIG_MT_LIGHTNING_EVENTS=1',
            here/'accessories.cpp',app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp',app/'src/Protobuf.cpp',
            vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objs,*host.crypto_link_flags(),'-o',out/'accessories'],check=True)
        subprocess.run([out/'accessories'],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        # Compile the pre-existing native power mapping/age suite both ways through the normal adapter.
        flags.remove('-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1')
        flags.remove('-DCONFIG_MT_ONE_WIRE_TELEMETRY=1')
        subprocess.run([*host.cxx(),'-std=c++17',*flags,app/'tests/sensor_producer_tests.cpp','-o',out/'native'],check=True)
        subprocess.run([out/'native'],check=True)
