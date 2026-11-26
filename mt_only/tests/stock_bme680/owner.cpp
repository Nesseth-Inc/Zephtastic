// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/bme680/ZephyrBme680.cpp"
#include <cassert>
#include <vector>
using namespace mt::stock::telemetry::bme680;
uint8_t gasFlags=0x30,waitRegister=0,waitValue=0;
bool sensorReady=false;unsigned initWait=0;int busError=0;unsigned fetches=0;
float readings[]={24.5f,40,101.325f,500000};
static std::vector<uint8_t> record;static int ioError=0;
ssize_t settings_get_val_len(const char *name){assert(!strcmp(name,"mtxsensor/bme680"));return ioError?ioError:ssize_t(record.size());}
ssize_t settings_load_one(const char *name,void *p,size_t n){assert(!strcmp(name,"mtxsensor/bme680"));if(ioError)return ioError;
    if(n!=record.size())return -EILSEQ;memcpy(p,record.data(),n);return ssize_t(n);}
int settings_save_one(const char *name,const void *p,size_t n){assert(!strcmp(name,"mtxsensor/bme680"));if(ioError)return ioError;
    const auto *bytes=static_cast<const uint8_t*>(p);record.assign(bytes,bytes+n);return 0;}
int settings_delete(const char *name){assert(!strcmp(name,"mtxsensor/bme680"));if(ioError)return ioError;record.clear();return 0;}
int main(){
    Reading r{};assert(measure(r)&&fetches==1&&waitRegister==0x64&&waitValue==0x65&&sensorReady&&initWait==300);
    assert(r.temperature==24.5f&&r.humidity==40&&r.pressureHpa>1013&&r.pressureHpa<1014&&r.gasOhms==500000);
    for(auto flags:{0x00,0x10,0x20}){gasFlags=flags;assert(measure(r)&&r.gasOhms==0);}
    busError=-EIO;assert(!measure(r));busError=0;gasFlags=0x30;
    enable(true);assert(enabled);assert(loadState(0,1800000000));
    for(unsigned i=0;i<100;i++){assert(measure(r));sampling.accept(r,i*60000);checkpoint(i*60000,1800000000+i*60);}
    assert(record.size()==sizeof(BME680IaqState));zt::telemetry::EnvironmentReadings out{};merge(out,99*60000);
    const auto m=zt::telemetry::toMeshtasticEnvironmentMetrics(out);assert(m.has_iaq&&m.iaq==0&&m.has_gas_resistance&&m.gas_resistance==500);
    enable(false);out={};merge(out,99*60000);assert(!out.bmeIaq);
    ioError=-EIO;assert(!loadState(0,1800000000)&&retire()==-EIO);
    ioError=0;assert(!retire()&&record.empty());enable(true);assert(!enabled&&retired);
}
