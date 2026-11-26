#!/usr/bin/env python3
"""Compare the real Zephyr producer to selected stock methods on both nanopb ABIs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
here=Path(__file__).resolve().parent
root=here.parents[2]
sys.path.insert(0,str(root))
import host_test_toolchain as host
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stock',type=Path,required=True)
p.add_argument('--adafruit',type=Path,required=True)
args=p.parse_args()
def source(name):
    return subprocess.check_output(['git','-C',args.stock,'show','8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c:src/modules/Telemetry/'+name+'.cpp'],text=True)
def method(text,signature):
    start=text.index(signature);return text[start:text.index('\n}',start)+2]
proto=root/'zephcore/bench/upstream-differential/source'
with tempfile.TemporaryDirectory(prefix='zt-ina-') as work:
    out=Path(work);(out/'meshtastic').symlink_to(proto,target_is_directory=True)
    vendor=subprocess.check_output(['git','-C',args.adafruit,'show','1d14a8e31336eb155ffba14c92cbbe1ed3b2a0d3:Adafruit_INA260.cpp'],text=True)
    (out/'vendor-values.inc').write_text('\n'.join(method(vendor,'float Adafruit_INA260::'+name) for name in ('readCurrent(', 'readBusVoltage(')))
    selector=out/'select'
    for name in ('zephyr/device.h','zephyr/drivers/sensor.h','zephyr/devicetree.h'):
        path=selector/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('')
    for name in ('zephyr/device.h','zephyr/drivers/i2c.h','zephyr/drivers/sensor.h','zephyr/kernel.h','zephyr/sys/byteorder.h'):
        path=out/name;path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to(here/'driver_fixture.h')
    methods=[]
    for model in (219,226,260,3221):
        name=f'INA{model}Sensor';s=source('Sensor/'+name)
        for func in (['getEnvironmentMetrics','getPowerMetrics','getMetrics'] if model in (226,3221) else ['getMetrics']):
            methods.append(method(s,'bool '+name+'::'+func))
    for module in ('Environment','Power'):
        text=method(source(module+'Telemetry'),'bool '+module+'TelemetryModule::get'+module+'Telemetry')
        start=text.index('if (ina219Sensor.hasSensor())')
        bodies=text[start:text.index('if (max17048Sensor.hasSensor())',start)]
        methods.append('static void donor'+module+'(meshtastic_Telemetry *m){bool valid=false,hasSensor=false,get_metrics=false;'+bodies+'}')
    (out/'methods.inc').write_text('\n'.join(methods))
    for abi in ([],['-fshort-enums']):
        flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*abi,
               '-DCONFIG_MT_ENVIRONMENT_SENSORS=1','-DCONFIG_MT_INA260=1','-DCONFIG_MT_STOCK_INA_METRICS=1',
               *['-I'+str(x) for x in (out,root/'mt_only/src',root/'zephcore/adapters/sensors',root/'zephcore/helpers',proto/'nanopb')]]
        subprocess.run([*host.cc(),'-std=c11',*flags,'-c',root/'mt_only/src/power/ZephyrIna260.c','-o',out/'ina.o'],check=True)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,here/'driver.cpp',out/'ina.o','-o',out/'driver'],check=True)
        subprocess.run([out/'driver'],check=True)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,root/'mt_only/tests/power_metrics_adapter_tests.cpp','-o',out/'power'],check=True)
        subprocess.run([out/'power'],check=True)
        objs=[]
        for name in ('pb_common','pb_encode'):
            obj=out/(name+'.o');subprocess.run([*host.cc(),'-std=c11',*flags,'-c',proto/'nanopb'/(name+'.c'),'-o',obj],check=True);objs.append(obj)
        for selected in (1,2,3):
            subprocess.run([*host.cxx(),'-std=c++17',*flags,'-DCONFIG_MT_INA3221_ENV_CHANNEL='+str(selected),here/'cases.cpp',
                            root/'mt_only/src/environment/ZephyrEnvironment.cpp',root/'mt_only/src/Protobuf.cpp',*objs,'-o',out/'cases'],check=True)
            subprocess.run([out/'cases'],check=True)
            subprocess.run([*host.cxx(),'-std=c++17','-I'+str(selector),*flags,'-DCONFIG_MT_INA3221_BAT_CHANNEL='+str(selected),
                            here/'selection.cpp','-o',out/'selector'],check=True)
            subprocess.run([out/'selector'],check=True)
        subprocess.run([*host.cxx(),'-std=c++17',*flags,'-UCONFIG_MT_INA260','-DCONFIG_MT_SINGLE_INA219=1',
                        '-DCONFIG_MT_INA3221_ENV_CHANNEL=1',here/'cases.cpp',root/'mt_only/src/environment/ZephyrEnvironment.cpp',
                        root/'mt_only/src/Protobuf.cpp',*objs,'-o',out/'single'],check=True)
        subprocess.run([out/'single'],check=True)
