#!/usr/bin/env python3
"""Compare the actual T096 percentage interpolators and check the XIAO stock input."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root))
import host_test_toolchain as host
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
args = parser.parse_args()
pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
def stock(path):
    return subprocess.check_output(['git','-C',args.stock,'show',pin+':'+path],text=True)

cpp=stock('src/Power.cpp')
method=cpp[cpp.index('    virtual int getBatteryPercent() override',cpp.index('class AnalogBatteryLevel :')):]
method=method[:method.index('\n    }')+6].replace('override','')
defs=stock('src/Power.h')
curve=defs[defs.index('#ifndef NUM_OCV_POINTS'):defs.index('/*Note: 12V')]
t096=stock('variants/nrf52840/heltec_mesh_node_t096/variant.h')
xiao=stock('variants/esp32s3/seeed_xiao_s3/variant.h')
assert 'OCV_ARRAY' not in t096 and '#define ADC_MULTIPLIER (4.916F)' in t096
assert '#define BATTERY_PIN -1' in xiao and '#define ADC_CHANNEL ADC_CHANNEL_0' in xiao
assert 'adc_oneshot_read(adc_handle, adc_channel, &val)' in cpp and 'raw = espAdcRead()' in cpp
test='''#include <algorithm>
#include <cassert>
#include <cstdio>
#include "battery_curve.h"
using std::clamp;
#define NUM_CELLS 1
'''+curve+'''
class Donor { public:
    const uint16_t OCV[NUM_OCV_POINTS]={OCV_ARRAY};
    unsigned mv=0;
    const float noBatVolt=OCV[NUM_OCV_POINTS-1]-500;
    int getBattVoltage() const{return mv;}
'''+method+'''
};
int main(){
    Donor donor;
    for(donor.mv=2600;donor.mv<=4500;donor.mv++)
        assert(donor.getBatteryPercent()==battery_curve_lookup(&battery_curve_default,donor.mv));
    puts("PASS pinned T096 OCV against 1901 integer mV inputs, donor XIAO ADC1_CH0 selection");
}
'''
with tempfile.TemporaryDirectory(prefix='zt-stock-battery-') as folder:
    out=Path(folder)
    (out/'test.cpp').write_text(test)
    src=root/'zephcore/helpers'
    subprocess.run([*host.cc(),'-std=c11','-c',src/'battery_curve.c','-o',out/'curve.o'],check=True)
    subprocess.run([*host.cxx(),'-std=c++17','-I'+str(src),out/'test.cpp',out/'curve.o','-o',out/'test'],check=True)
    subprocess.run([out/'test'],check=True)
