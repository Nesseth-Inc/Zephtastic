#!/usr/bin/env python3
"""Compare the gauge's two Meshtastic producers against the pinned C++ sources."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
sys.path.insert(0, str(root))
import host_test_toolchain as host

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
parser.add_argument('--adafruit', type=Path, required=True)
args = parser.parse_args()
def source(repo, rev, path):
    return subprocess.check_output(['git', '-C', repo, 'show', rev + ':' + path], text=True)

def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

stock = source(args.stock, '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c', 'src/modules/Telemetry/Sensor/MAX17048Sensor.cpp')
driver = source(args.adafruit, '4953d76cc98fc3f2b072b477d80519b32f1f59dc', 'Adafruit_MAX1704X.cpp')
with tempfile.TemporaryDirectory(prefix='zt-max17048-') as directory:
    out = Path(directory)
    proto = root/'zephcore/bench/upstream-differential/source'
    (out/'meshtastic').symlink_to(proto, target_is_directory=True)
    bodies = [method(driver, 'float Adafruit_MAX17048::' + f) for f in ('cellVoltage(', 'cellPercent(', 'chargeRate(')]
    for name in ('isBatteryCharging()', 'isExternallyPowered()', 'isBatteryConnected()'):
        bodies.append(method(stock, 'bool MAX17048Singleton::' + name))
    bodies.append(method(stock, 'uint16_t MAX17048Singleton::getBusVoltageMv()'))
    bodies.append(method(stock, 'uint8_t MAX17048Singleton::getBusBatteryPercent()'))
    device = source(args.stock, '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c', 'src/modules/Telemetry/DeviceTelemetry.cpp')
    bodies.append(method(device, 'meshtastic_Telemetry DeviceTelemetryModule::getDeviceTelemetry()'))
    bodies.append(method(stock, 'bool MAX17048Sensor::getMetrics('))
    (out/'donor.inc').write_text('\n'.join(bodies))
    for abi in ([], ['-fshort-enums']):
        flags = [*host.cxx(), '-std=c++17', '-DCONFIG_MT_MAX17048=1', '-g', '-O1', '-fsanitize=address,undefined', *abi,
                '-I'+str(root/'mt_only/src'), '-I'+str(out), '-I'+str(proto/'nanopb'), here/'cases.cpp', '-o', out/'cases']
        subprocess.run(flags, check=True)
        subprocess.run([out/'cases'], check=True)
        # Link the actual Zephyr hardware owner against the same board-level ADC
        # and regulator mocks used by the common hardware telemetry regression.
        fixture=root/'mt_only/tests/hardware_metrics'
        helper=root/'zephcore/helpers'
        for adc in (0,1):
            common=['-O1','-g','-fsanitize=address,undefined','-DCONFIG_MT_MAX17048=1',
                    '-DMT_MOCK_HAS_ADC='+str(adc),*abi,*(['-DCONFIG_SOC_SERIES_NRF52=1'] if adc else []),
                    *['-I'+str(p) for p in (fixture/'mocks',fixture,helper,root/'mt_only/src',out,proto/'nanopb')]]
            objects=[]
            for src in (fixture/'mock_api.c',helper/'battery_curve.c'):
                obj=out/(src.stem+'.o')
                subprocess.run([*host.cc(),'-std=c11',*common,'-c',src,'-o',obj],check=True)
                objects.append(obj)
            subprocess.run([*host.cxx(),'-std=c++17',*common,here/'owners.cpp',root/'mt_only/src/ZephyrHardware.cpp',
                            *objects,'-o',out/'owners'],check=True)
            subprocess.run([out/'owners'],check=True)
