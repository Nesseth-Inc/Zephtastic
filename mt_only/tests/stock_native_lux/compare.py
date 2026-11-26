#!/usr/bin/env python3
"""Exhaustive raw-word comparison for declared native BH1750 and OPT3001 light channels."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parents[2]))
import host_test_toolchain as host
args = argparse.ArgumentParser(description=__doc__)
for name in ('zephyr','bh1750','opt3001-archive','stock'):
    args.add_argument('--'+name, required=True, type=Path)
args = args.parse_args()

def source(repo, rev, filename):
    return subprocess.check_output(['git','-C',repo,'show',rev+':'+filename],text=True)

def fn(src, decl):
    start = src.index(decl)
    return src[start:src.index('\n}',start)+2]

stock = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
sensor = 'src/modules/Telemetry/Sensor/'
bh_stock = source(args.stock,stock,sensor+'BH1750Sensor.cpp')
opt_stock = source(args.stock,stock,sensor+'OPT3001Sensor.cpp')
assert '#define BH1750_SENSOR_MODE BH1750Mode::CHM' in bh_stock
assert all(s in opt_stock for s in ('RangeNumber = 0b1100','ConvertionTime = 0b0','Latch = 0b1','ModeOfConversionOperation = 0b11'))
bh_driver = source(args.bh1750,'fcd30fea797383e5c29672549a65f97754fcd099','src/BH1750_WE.cpp')
assert 'setMeasuringTimeFactor(1.0)' in bh_driver
assert hashlib.sha256(args.opt3001_archive.read_bytes()).hexdigest() == '95d3878e32ead6be60ade8636e0de8758244f1386f7d643e9130e54b1f73d8b4'
with tarfile.open(args.opt3001_archive) as pkg:
    name=next(f for f in pkg.getnames() if f.endswith('src/ClosedCube_OPT3001.cpp'))
    opt_driver=pkg.extractfile(name).read().decode()

with tempfile.TemporaryDirectory(prefix='zt-native-lux-') as tmp:
    output=Path(tmp)
    objects=[]
    for model, stem, path, digest in (
        ('bh1750','bh1750','drivers/sensor/rohm/bh1750/bh1750.c','7ed73dfe76b878b70db42c9d3c70dafc08748e5d7f3eef7e8863d66fa3603a13'),
        ('opt3001','opt300x','drivers/sensor/ti/opt300x/opt300x.c','02dc89525a44f86a519fb21c60d603cd0b3ceb6ab73a09ae6dc53b57cc3f09b7')):
        data=(args.zephyr/path).read_bytes()
        assert hashlib.sha256(data).hexdigest()==digest
        native=data.decode()
        assert 'return BH1750_ONE_TIME_HIGH_RES_MODE;' in native if model=='bh1750' else 'OPT300X_CONVERSION_MODE_CONTINUOUS' in native
        c='''
#include <stdint.h>
#include <errno.h>
struct device {const void *config; void *data;};
struct sensor_value{int val1,val2;};enum sensor_channel{SENSOR_CHAN_LIGHT,SENSOR_CHAN_ALL};
#define OPT300X_SAMPLE_EXPONENT_SHIFT 12
#define OPT300X_MANTISSA_MASK 0xfff
#define BH1750_DEFAULT_MTREG 69
#define BH1750_HIGH_RES_2_DTS_ENUM 2
struct bh1750_dev_config {unsigned mtreg,resolution;};
struct bh1750_data {uint16_t sample;};
struct opt300x_data {uint16_t sample;};
'''+fn(native,'static int '+stem+'_channel_get(')+'''
float native_MODEL(uint16_t raw){
    struct bh1750_dev_config cfg={69,1};struct STEM_data data={raw};
    struct device dev={&cfg,&data};struct sensor_value value={0};
    if(STEM_channel_get(&dev,SENSOR_CHAN_LIGHT,&value)) return -1;
    return (float)value.val1+(float)value.val2/1000000.f;
}'''.replace('STEM',stem).replace('MODEL',model)
        src=output/(stem+'.c');src.write_text(c)
        obj=output/(stem+'.o');objects.append(obj)
        subprocess.run([*host.cc(),'-std=c11','-O1','-fsanitize=undefined','-c',src,'-o',obj],check=True)
    code='''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
extern "C" float native_bh1750(uint16_t), native_opt3001(uint16_t);
struct BH1750_WE {
 enum Mode { CHM,CHM_2,OTH_2 };Mode deviceMode=CHM; float lux=0,mtf=1;uint16_t sample=0;
 uint16_t readBH1750_WE(){return sample;}float getLux();
};
'''+fn(bh_driver,'float BH1750_WE::getLux(')+'''
enum OPT3001_Commands {RESULT};enum OPT3001_ErrorCode {NO_ERROR};
union OPT3001_ER {uint16_t rawData;struct{uint16_t Result:12;uint8_t Exponent:4;};};
struct OPT3001 {float lux;OPT3001_ErrorCode error;OPT3001_ER raw;};
struct ClosedCube_OPT3001 {
 uint16_t sample=0; OPT3001_ErrorCode writeData(OPT3001_Commands){return NO_ERROR;}
 OPT3001_ErrorCode readData(uint16_t *v){*v=sample;return NO_ERROR;}
 OPT3001 returnError(OPT3001_ErrorCode){return {};} OPT3001 readRegister(OPT3001_Commands);
};
'''+fn(opt_driver,'OPT3001 ClosedCube_OPT3001::readRegister(')+'''
int main() {
 BH1750_WE bh;ClosedCube_OPT3001 opt;float worstBh=0,worstOpt=0;
 for(unsigned w=0;w<65536;++w){
    bh.sample=opt.sample=w;
    float errBh=std::abs(bh.getLux()-native_bh1750(w));
    float errOpt=std::abs(opt.readRegister(RESULT).lux-native_opt3001(w));
    worstBh=std::fmax(worstBh,errBh);worstOpt=std::fmax(worstOpt,errOpt);
 }
 assert(worstBh<0.011f && worstOpt<0.00001f);
 printf("PASS all 65536 input words per native photometric driver, max differences BH1750 %.8f lx / OPT3001 %.8f lx\\n",worstBh,worstOpt);
}
'''
    cpp=output/'models.cpp';cpp.write_text(code)
    exe=output/'compare'
    subprocess.run([*host.cxx(),'-std=c++17','-O1','-fsanitize=undefined',cpp,*objects,'-o',exe],check=True)
    subprocess.run([exe],check=True)
