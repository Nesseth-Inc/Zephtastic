#!/usr/bin/env python3
"""Compare two native Zephyr channel conversions with the corresponding stock sensors."""
import argparse
import hashlib
import re
from pathlib import Path
import subprocess
import sys
import tempfile

here=Path(__file__).resolve().parent
root=here.parents[2]
sys.path.insert(0,str(root))
import host_test_toolchain as host
parser=argparse.ArgumentParser(description=__doc__)
for option in ('stock','pct2075','mcp9808','zephyr'):
    parser.add_argument('--'+option,type=Path,required=True)
args=parser.parse_args()
def git(repo,rev,path):
    return subprocess.check_output(['git','-C',repo,'show',rev+':'+path],text=True)
def sdk(path,sha):
    data=(args.zephyr/'drivers/sensor'/path).read_bytes()
    assert hashlib.sha256(data).hexdigest()==sha
    return data.decode()
def body(data,signature):
    start=data.index(signature)
    return data[start:data.index('\n}',start)+2]

lm=sdk('lm75/lm75.c','47283cf20018ed0b2259118c7db93c733a28f8796ca26deb3290895f067be2f4')
jc=sdk('jedec/jc42/jc42.c','2f02ee43c2da6b8fd19d6948cea5ff28408666fdb0bd99def98e976aaaed5845')
header=sdk('jedec/jc42/jc42.h','7966ffae1a2a4c7d9490c3d5eea1141850f5385da64c64585df9be33a73b46cf')
parts=[body(lm,'static void lm75_temp_to_sensor_value'),body(header,'static inline int jc42_temp_signed_from_reg'),body(jc,'static int jc42_channel_get')]
parts.append(body(git(args.pct2075,'f48d4fa87adbde3a94883f0416bba6278f4677a5','Adafruit_PCT2075.cpp'),'float Adafruit_PCT2075::getTemperature('))
parts.append(body(git(args.mcp9808,'1447e4e8ab099dec3bc76bbb19ab9136f9478b15','Adafruit_MCP9808.cpp'),'float Adafruit_MCP9808::readTempC()'))
for sensor in ('PCT2075','MCP9808'):
    data=git(args.stock,'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','src/modules/Telemetry/Sensor/'+sensor+'Sensor.cpp')
    parts.append(body(data,'bool '+sensor+'Sensor::getMetrics('))

proto=root/'zephcore/bench/upstream-differential/source'
with tempfile.TemporaryDirectory(prefix='zt-ambient-') as temp:
    out=Path(temp);(out/'meshtastic').symlink_to(proto,target_is_directory=True)
    (out/'donors.inc').write_text('\n'.join(parts))
    owners = {
        'SPA06':'Spa06', 'DFRobotLark':'Lark', 'AHT10':'Aht', 'BMP085':'Bmp085',
        'BME280':'Bme280','BME680':'Bme680','BMP280':'Bmp280','DPS310':'Dps310',
        'MCP9808':'Mcp9808','LPS22HB':'Lps22hb','MLX90632':'Mlx90632',
        'BMP3XX':'Bmp3xx','PCT2075':'Pct2075','SHTXX':'Sht','DS248X':'OneWire',
    }
    pin='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
    constructor=git(args.stock,pin,'src/modules/Telemetry/EnvironmentTelemetry.cpp')
    rows=[]
    for name in re.findall(r'addSensor<(\w+)Sensor>',constructor):
        if name not in owners:
            continue
        path='src/modules/Telemetry/Sensor/'+name+('' if name=='AHT10' else 'Sensor')+'.cpp'
        function=body(git(args.stock,pin,path),'bool '+name+'Sensor::getMetrics')
        flags=[]
        for field,bit in [('temperature','Temperature'),('relative_humidity','Humidity'),('barometric_pressure','Pressure')]:
            if re.search(r'has_'+field+r'\s*=\s*true',function):
                flags.append(bit)
        if re.search(r'has_relative_humidity\s*=\s*false',function):
            flags.append('ClearHumidity')
        assert flags and 'Temperature' in flags, name
        rows.append('{Source::'+owners[name]+','+'|'.join(flags)+'},')
    assert len(rows)==len(owners)
    (out/'registered.inc').write_text('\n'.join(rows))
    for abi in ([],['-fshort-enums']):
        subprocess.run([*host.cxx(),'-std=c++17','-O1','-g','-fsanitize=address,undefined',*abi,
                        '-DCONFIG_MT_STOCK_ENVIRONMENT_CHANNELS=1','-I'+str(out),
                        '-I'+str(root/'mt_only/tests/stock_sensor_channels/mocks'),'-I'+str(root/'mt_only/src'),
                        '-I'+str(proto/'nanopb'),here/'cases.cpp','-o',out/'test'],check=True)
        subprocess.run([out/'test'],check=True)
        subprocess.run([*host.cxx(),'-std=c++17','-O1','-fsanitize=address,undefined',*abi,
                        '-DCONFIG_MT_ACCESSORY_SENSORS=1','-DCONFIG_MT_LARK_WEATHER=1',
                        '-DCONFIG_MT_STOCK_BAROMETERS=1','-I'+str(out),'-I'+str(root/'mt_only/src'),
                        '-I'+str(proto/'nanopb'),here/'order.cpp','-o',out/'order'],check=True)
        subprocess.run([out/'order'],check=True)
