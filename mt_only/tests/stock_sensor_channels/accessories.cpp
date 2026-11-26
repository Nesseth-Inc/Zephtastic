// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "EnvironmentQuery.h"
#include "ModuleAnnouncement.h"
#include "ModuleController.h"
#include "sen_maintenance_oracle.h"
#include "range_oracle.h"
#include "light_oracle.h"
#include "weather_oracle.h"
#include "stock/telemetry/accessory/Rainfall.h"
#include "stock/telemetry/accessory/Ltr390.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/accessory/Scd30.h"
#include "stock/telemetry/accessory/RangeAndRadiation.h"
#include "stock/telemetry/accessory/Senxx.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <map>
#include <string>
using namespace mt::stock::telemetry::accessory;
namespace {
struct Frame { unsigned addr,op;std::vector<uint16_t> words;uint64_t at; };
std::vector<Frame> trace;
std::map<std::string,std::vector<uint8_t>> saved;
std::string sen5Name="SEN55",sen6Name="SEN66";bool corrupt=false,noNauData=false,scaleAfeFail=false;
bool failResetRead=false,lightBusStuck=false;unsigned busRecoveries=0;
std::string larkValue;unsigned larkStep=0,larkHumidityRequests=0;bool larkFails=false,larkHumidityFails=false;
std::string sensorSerial="SENSIRION-0123456789";
std::vector<uint16_t> vocState={0x1234,0xabcd,0x3333,0x4444};
uint16_t failure=0,last[128]{},interval=2,asc=1,correction=0x8012;
int32_t rawScale=-12000;uint32_t epoch=0;
uint8_t registers[128][256]{};bool saveFails=false;
size_t writes(unsigned a,unsigned command){size_t n=0;for(auto &f:trace)n+=f.addr==a&&f.op==command;return n;}
const Frame &latest(unsigned a,unsigned command){
    for(auto it=trace.rbegin();it!=trace.rend();++it)if(it->addr==a&&it->op==command)return *it;
    assert(false);return trace.front();
}
struct Port:Bus{
    i2c_dt_spec spec;explicit Port(unsigned address):spec{uint8_t(address)}{}
    int write(const uint8_t *p,size_t n)override{return i2c_write_dt(&spec,p,n);}
    int read(uint8_t *p,size_t n)override{return i2c_read_dt(&spec,p,n);}
    int readReg(uint8_t r,uint8_t *p,size_t n)override{return i2c_write_read_dt(&spec,&r,1,p,n);}
    void wait(unsigned ms)override{k_msleep(ms);}uint64_t now()override{return k_uptime_get();}
    int recover()override{return i2c_recover_bus(nullptr);}
    uint32_t utc()override{return epoch;}
    int save(const char *k,const void *p,size_t n)override{
        if(saveFails)return -EIO;auto q=(const uint8_t *)p;saved[k]={q,q+n};return 0;
    }
    int load(const char *k,void *p,size_t n)override{
        if(!saved.count(k))return -ENOENT;if(saved[k].size()!=n)return -EINVAL;memcpy(p,saved[k].data(),n);return 0;
    }
};
}
int i2c_recover_bus(void *){++busRecoveries;lightBusStuck=false;return 0;}
bool i2c_is_ready_dt(const i2c_dt_spec *){return true;}
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){
    const auto a=s->address;
    if(a==0x42){
        assert(n>=4&&p[0]==0&&p[2]==0&&n==size_t(p[1]+3));std::string key((const char *)p+3,p[1]);
        static const std::map<std::string,std::string> values={{"Temp"," -7.75"},{"Pressure","1003.5"},{"Speed","7.25"},{"Dir","SSE"},{"Humi","81.25"}};
        if(key=="Humi"){++larkHumidityRequests;if(larkHumidityFails)return -EIO;}
        larkStep=0;larkValue=values.at(key);return larkFails?-EIO:0;
    }
    if(a==0x66){assert(n==1&&*p==6);trace.push_back({a,*p,{},k_uptime_get()});return failure==0x661?-EIO:0;}
    if(a==0x57){assert(n==1&&*p==1);trace.push_back({a,*p,{},k_uptime_get()});return failure==0x571?-EIO:0;}
    if(a==0x1d){
        assert((n==1&&(*p==0||*p==0x0c))||(n==2&&p[0]==0x26&&(p[1]==1||p[1]==24)));
        if(n==1){last[a]=*p;return 0;}
        registers[a][0x26]=p[1];trace.push_back({a,p[0],{p[1]},k_uptime_get()});return 0;
    }
    assert(n>=2);
    if(a==0x53){
        assert(n==2);if(failure==0x530+p[0])return -EIO;
        if(p[0]==0&&(p[1]&0x10)&&failResetRead){lightBusStuck=true;failResetRead=false;}
        registers[a][p[0]]=(p[0]==0&&p[1]&0x10)?0:p[1];return 0;
    }
    if(a==3||a==0x2a){
        assert(n==2);trace.push_back({a,p[0],{p[1]},k_uptime_get()});
        if(failure==0x100+p[0]){failure=0;return -EIO;}registers[a][p[0]]=p[1];return 0;
    }
    uint16_t op=uint16_t(p[0])<<8|p[1];Frame f{a,op,{},k_uptime_get()};
    assert((n-2)%3==0);for(size_t i=2;i<n;i+=3){
        assert(Bus::crc(p+i)==p[i+2]);f.words.push_back(uint16_t(p[i])<<8|p[i+1]);
    }
    trace.push_back(f);last[a]=op;
    if(failure&&failure==op){failure=0;return -EIO;}
    if(a==0x61&&op==0x4600&&!f.words.empty())interval=f.words[0];
    if((op==0x5306||op==0x6711)&&!f.words.empty())asc=f.words[0];
    return 0;
}
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *p,size_t n){
    auto a=s->address;unsigned op=last[a];std::vector<uint16_t> values;
    if(a==0x42){
        switch(larkStep++){
            case 0:assert(n==1);*p=0x53;break;
            case 1:assert(n==1);*p=0;break;
            case 2:assert(n==2);p[0]=larkValue.size();p[1]=0;break;
            case 3:assert(n==larkValue.size());memcpy(p,larkValue.data(),n);break;
            default:assert(false);
        }
        return 0;
    }
    if(a==0x1d){
        assert(n==4);if(!last[a]){memcpy(p,registers[a],4);return 0;}
        assert(last[a]==0x0c&&k_uptime_get()-latest(a,0x26).at>=100);
        if(failure==registers[a][0x26])return -EIO;
        uint32_t data=registers[a][0x26]==1?0x00010203:0x01020304;
        for(unsigned i=0;i<4;i++)p[i]=uint8_t(data>>(8*i));return 0;
    }
    if(a==0x66){
        assert(n==3&&latest(a,6).op==6);
        if(failure==0x660)return -EIO;
        memcpy(p,registers[a]+6,n);return 0;
    }
    if(a==0x57){
        assert(n==3&&k_uptime_get()-latest(a,1).at>=100);
        if(failure==0x572)return -EIO;
        memcpy(p,registers[a],n);return 0;
    }
    if(op==0xd014||op==0xd033){
        std::string model=op==0xd033?sensorSerial:(a==0x69?sen5Name:sen6Name);model.resize(32,0);
        for(int i=0;i<32;i+=2)values.push_back(uint16_t(model[i])<<8|model[i+1]);
    }else if(op==0x6181)values=vocState;
    else if(op==0x0202)values={1};
    else if(op==0x5306||op==0x6711)values={asc};
    else if(op==0x6707)values={correction};
    else if(op==0x4600)values={interval};
    else if(a==0x61&&op==0x300){
        for(float x:{507.5f,24.75f,48.5f}){uint32_t bits;memcpy(&bits,&x,4);values.push_back(bits>>16);values.push_back(bits);}
    }else if(op==0x416||op==0x316)values={1500,1600,1700,1800,2000};
    else if(op==0x413)values={20,60,70,90,1500,1600,1700,1800,2000,1000};
    else if(op==0xd206)values={0,16};
    else{values={100,200,300,400,5000,5000,360,470,120,600};values.resize(n/3);}
    assert(n==values.size()*3);for(size_t i=0;i<values.size();i++){
        p[3*i]=values[i]>>8;p[3*i+1]=values[i];p[3*i+2]=Bus::crc(p+3*i);
    }
    if(corrupt)p[n-1]^=1;return 0;
}
int i2c_write_read_dt(const i2c_dt_spec *s,const uint8_t *tx,size_t len,uint8_t *out,size_t n){
    assert(len==1);auto a=s->address;uint8_t reg=*tx;
    if(a==0x53&&lightBusStuck)return -EIO;
    assert(a!=0x66); // The RadSens register select and read must be separate transactions.
    memcpy(out,registers[a]+reg,n);
    if(a==0x2a&&reg==0)out[0]=(out[0]&~40)|8|(noNauData?0:32);
    if(a==0x2a&&reg==2){registers[a][2]&=~4;out[0]&=~12;if(scaleAfeFail)out[0]|=8;}
    if(a==0x2a&&reg==0x12){assert(n==3);uint32_t bits=rawScale;out[0]=bits>>16;out[1]=bits>>8;out[2]=bits;}
    if(a==3&&reg==3)registers[a][3]&=0xf0;
    return 0;
}
ssize_t settings_get_val_len(const char *k){return saved.count(k)?ssize_t(saved.at(k).size()):-ENOENT;}
ssize_t settings_load_one(const char *k,void *p,size_t n){Port b(0);return b.load(k,p,n)?-EIO:ssize_t(n);}
int settings_save_one(const char *k,const void *p,size_t n){Port b(0);return b.save(k,p,n);}
static void protocols(){
    Port cd(0x61);Scd30 scd(cd);meshtastic_AirQualityMetrics m{};
    assert(scd.measure(m)==-EAGAIN&&scd.measure(m)==0&&m.co2==507&&m.co2_temperature==24.75);
    meshtastic_SCD30_config q{};q.has_set_asc=true;q.has_set_target_co2_conc=true;q.set_target_co2_conc=435;asc=1;failure=0x5204;
    assert(scd.apply(q)==-EIO&&asc==1);assert(scd.apply(q)==0&&asc==0);
    q={};q.has_set_altitude=true;q.set_altitude=134;q.has_set_temperature=true;q.set_temperature=22.25;
    q.has_set_measurement_interval=true;q.set_measurement_interval=22;
    assert(scd.apply(q)==0&&latest(0x61,0x5102).words[0]==134&&latest(0x61,0x5403).words[0]==250&&interval==22);
    q.set_measurement_interval=1801;auto n=trace.size();assert(scd.apply(q)==-EINVAL&&n==trace.size());
    q={};q.has_set_temperature=true;q.set_temperature=100;assert(scd.apply(q)==0&&latest(0x61,0x5403).words[0]==0);
    q={};q.has_soft_reset=q.soft_reset=true;assert(scd.apply(q)==0&&latest(0x61,0x10).at-latest(0x61,0xd304).at==2000);
    corrupt=true;assert(scd.measure(m)==-EIO);corrupt=false;
    puts("PASS SCD30 wire values, interval restart/readback, forced calibration rollback, temperature sentinel, CRC and reset recovery");
    Port five(0x69),six(0x6b);
    for(auto name:{"SEN50","SEN54","SEN55","SEN62","SEN63C","SEN65","SEN66","SEN68","SEN69C"}){
        const bool familySix=name[3]=='6';(familySix?sen6Name:sen5Name)=name;
        const auto addr=familySix?0x6b:0x69;Senxx sensor(familySix?six:five,familySix);
        meshtastic_SensorConfig cfg{};cfg.has_sen5x_config=!familySix;cfg.has_sen6x_config=familySix;
        cfg.sen5x_config.has_set_one_shot_mode=cfg.sen6x_config.has_set_one_shot_mode=true;
        cfg.sen5x_config.set_one_shot_mode=cfg.sen6x_config.set_one_shot_mode=false;
        assert(sensor.apply(cfg)==0);m={};assert(sensor.measure(m,60)==-EAGAIN);k_msleep(15000);
        assert(sensor.measure(m,60)==0&&m.pm40_standard==30&&m.particles_05um==15000&&m.particles_40um==1000);
        assert(m.has_particles_tps==!familySix);assert(m.has_pm_temperature==(std::string(name)!="SEN50"));
        if(familySix)cfg.sen6x_config={};else cfg.sen5x_config={};
        cfg.sen5x_config.has_set_temperature=cfg.sen6x_config.has_set_temperature=true;
        cfg.sen5x_config.set_temperature=cfg.sen6x_config.set_temperature=27;
        if(std::string(name)=="SEN50")assert(sensor.apply(cfg)==-ENOTSUP);
        else {assert(sensor.apply(cfg)==0);auto &f=latest(addr,0x60b2);assert(f.words.size()==(familySix?4:3)&&f.words[0]==uint16_t(-400));}
        if(familySix){
            bool carbon=std::string(name)=="SEN63C"||std::string(name)=="SEN66"||std::string(name)=="SEN69C";
            auto &c=cfg.sen6x_config;c={};c.has_set_asc=c.has_set_target_co2_conc=true;c.set_target_co2_conc=430;asc=1;
            correction=65535;assert(sensor.apply(cfg)==(carbon?-EIO:-ENOTSUP)&&asc==1);correction=0x8012;
            if(carbon){
                assert(sensor.apply(cfg)==0&&asc==0);
                c={};c.has_set_ambient_pressure=true;c.set_ambient_pressure=101325;assert(sensor.apply(cfg)==0&&latest(addr,0x6720).words[0]==1013);
                c.has_set_altitude=true;c.set_altitude=700;auto p=writes(addr,0x6720);assert(sensor.apply(cfg)==0&&writes(addr,0x6720)==p);
                c={};c.has_factory_reset=c.factory_reset=true;assert(sensor.apply(cfg)==0&&latest(addr,0x21).at-latest(addr,0x6754).at>=1400);
            }
        }
        cfg.sen5x_config={};cfg.sen6x_config={};
        cfg.sen5x_config.has_start_fan_cleaning=cfg.sen5x_config.start_fan_cleaning=true;
        cfg.sen6x_config.has_start_fan_cleaning=cfg.sen6x_config.start_fan_cleaning=true;
        assert(sensor.apply(cfg)==0);assert(sensor.apply(cfg)==-EBUSY);
        assert(sensor.measure(m,60)==-EBUSY);k_msleep(10500);sensor.maintenance();
    }
    sen6Name="SEN55";Senxx wrong(six,true);m={};assert(wrong.measure(m,60)==-ENODEV);sen6Name="SEN66";
    puts("PASS nine SEN variants, product gates, PM bin units, warm-up, SEN54/55 and SEN6x temperature, cleaning, ASC/FRC/altitude/pressure/reset");
    Port as(3),nau(0x2a);Lightning l(as);Scale scale(nau);Environment env;
    registers[3][8]=0x10;assert(l.tune(56)==0&&registers[3][8]==0x17);
    assert(l.tune(121)==-EINVAL);saveFails=true;assert(l.tune(80)==-EIO&&registers[3][8]==0x17);saveFails=false;
    registers[3][3]|=8;registers[3][7]=12;assert(l.measure(env)==0&&env.strikes==1&&env.distance==12&&env.hasDistance);
    k_msleep(3600000);env={};assert(l.measure(env)==0&&!env.strikes&&!env.hasDistance);
    assert(scale.measure(env)==-EAGAIN);assert(scale.set(0)==0);assert(scale.measure(env)==-EAGAIN);
    rawScale=8000;assert(scale.set(2)==0);rawScale=-2000;assert(scale.measure(env)==0&&env.weight==1);
    Scale rebooted(nau);assert(rebooted.measure(env)==0&&env.weight==1);
    noNauData=true;assert(rebooted.set(0)==-ETIMEDOUT);noNauData=false;assert(rebooted.measure(env)==0&&env.weight==1);
    scaleAfeFail=true;Scale broken(nau);assert(broken.set(0)==-EIO);scaleAfeFail=false;
    puts("PASS AS3935 verified trim, persistence rollback and expiring strikes; NAU7802 signed samples, 64-reading tare/calibration, power, kg, timeout and reboot");
}
static void senMaintenance(){
    Port bus(0x6b);
    constexpr uint32_t baseline=1600000000;
    stockSensorOracle::SENXXSensor oracle;oracle.vocTime=baseline;
    assert(!oracle.vocStateValid());oracle.vocState[7]=1;assert(oracle.vocStateValid());
    assert(!oracle.vocStateRecent(baseline-1)&&oracle.vocStateRecent(baseline+599)&&!oracle.vocStateRecent(baseline+600));
    oracle.nowMs=3600999;assert(!oracle.vocStateStable());++oracle.nowMs;assert(oracle.vocStateStable());
    const char *key="mtxsensor/sen6hist";
    sen6Name="SEN62";saved.erase(key);epoch=0;
    auto fanCount=writes(0x6b,0x5607);
    meshtastic_AirQualityMetrics m{};
    Senxx sensor(bus,true);
    assert(sensor.measure(m,60)==-EAGAIN&&!saved.count(key));
    epoch=SENXX_VOC_VALID_DATE;
    (void)sensor.measure(m,60);assert(!saved.count(key));
    epoch=baseline;
    (void)sensor.measure(m,60);assert(saved[key].size()==60&&writes(0x6b,0x5607)==fanCount);
    epoch=baseline-1;(void)sensor.measure(m,60);
    epoch=baseline+ONE_WEEK_IN_SECONDS;(void)sensor.measure(m,60);assert(writes(0x6b,0x5607)==fanCount);
    epoch++;failure=0x5607;assert(sensor.measure(m,60)==-EIO);
    assert(writes(0x6b,0x5607)==++fanCount);(void)sensor.measure(m,60);
    assert(writes(0x6b,0x5607)==fanCount);
    k_msleep(60000);assert(sensor.measure(m,60)==-EBUSY&&writes(0x6b,0x5607)==++fanCount);
    auto old=saved[key];k_msleep(SENXX_CLEANING_DURATION_MS-1);sensor.maintenance();assert(saved[key]==old);
    epoch+=10;k_msleep(1);saveFails=true;sensor.maintenance();assert(saved[key]==old);
    saveFails=false;(void)sensor.measure(m,60);assert(saved[key]==old);
    k_msleep(60000);sensor.maintenance();assert(saved[key]!=old&&writes(0x6b,0x5607)==fanCount);
    Senxx rebooted(bus,true);epoch+=604800;
    assert(rebooted.measure(m,60)==-EAGAIN&&writes(0x6b,0x5607)==fanCount);
    ++epoch;assert(rebooted.measure(m,60)==-EBUSY&&writes(0x6b,0x5607)==++fanCount);
    puts("PASS weekly SEN fan cleaning waits for credible UTC, strictly over seven days, 10.5s completion, retry/backoff and persistent completion across reboot");

    saved.erase(key);sen6Name="SEN66";epoch=baseline;
    meshtastic_SensorConfig config{};config.has_sen6x_config=true;config.sen6x_config.has_set_one_shot_mode=true;
    config.sen6x_config.set_one_shot_mode=false;
    Senxx continuous(bus,true);assert(continuous.apply(config)==0);
    assert(continuous.measure(m,60)==-EAGAIN);
    const auto started=bus.now();auto vocOps=writes(0x6b,0x6181);
    mockKernel::ms=started+3599000;assert(continuous.measure(m,60)==0&&writes(0x6b,0x6181)==vocOps);
    mockKernel::ms=started+3601000;assert(continuous.measure(m,60)==0&&writes(0x6b,0x6181)==++vocOps);
    assert(latest(0x6b,0x6181).words.empty());const auto snapshot=saved[key];
    assert(snapshot.size()==60);
    Senxx restarted(bus,true);epoch=0;assert(restarted.measure(m,60)==-EAGAIN);
    epoch=baseline+599;failure=0x6181;
    assert(restarted.measure(m,60)==-EIO&&latest(0x6b,0x6181).words==vocState);
    k_msleep(1000);(void)restarted.measure(m,60);
    assert(writes(0x6b,0x6181)==vocOps+2&&latest(0x6b,0x6181).words==vocState);
    const auto restored=writes(0x6b,0x6181);k_msleep(500);(void)restarted.measure(m,60);assert(writes(0x6b,0x6181)==restored);
    for(auto t:{baseline-1,baseline+600,baseline+10000}){
        saved[key]=snapshot;epoch=t;Senxx oldState(bus,true);
        assert(oldState.measure(m,60)==-EAGAIN&&writes(0x6b,0x6181)==restored);
    }
    epoch=baseline+5;saved[key]=snapshot;
    auto originalSerial=sensorSerial;sensorSerial="OTHER";Senxx swapped(bus,true);
    assert(swapped.measure(m,60)==-EAGAIN&&writes(0x6b,0x6181)==restored);
    sensorSerial=originalSerial;saved[key]=snapshot;sen6Name="SEN65";
    Senxx otherModel(bus,true);assert(otherModel.measure(m,60)==-EAGAIN&&writes(0x6b,0x6181)==restored);
    saved[key]=snapshot;saved[key].back()^=0x80;sen6Name="SEN66";Senxx damaged(bus,true);
    assert(damaged.measure(m,60)==-EAGAIN&&writes(0x6b,0x6181)==restored);
    saved[key]=snapshot;SenHistory rows(bus,key);assert(rows.identify(7)==0);
    auto state=vocState;vocState.assign(4,0);assert(rows.capture(epoch)==-ENODATA&&saved[key]==snapshot);
    vocState=state;corrupt=true;assert(rows.capture(epoch)==-EIO&&saved[key]==snapshot);corrupt=false;
    puts("PASS four-word SEN VOC capture after warm-up, exact CRC frames and late-clock restore before restart, NACK retry, 599/600s age boundary, time rollback, device identity and corrupt record rejection");

    saved.erase(key);config.sen6x_config.set_one_shot_mode=true;Senxx oneShot(bus,true);assert(oneShot.apply(config)==0);
    assert(oneShot.measure(m,60)==-EAGAIN);
    k_msleep(3601000);vocState.assign(4,0);auto stops=writes(0x6b,0x0104);
    assert(oneShot.measure(m,60)==0&&writes(0x6b,0x0104)==stops);
    assert(oneShot.measure(m,60)==0&&writes(0x6b,0x0104)==stops);
    k_msleep(1000);vocState=state;
    assert(oneShot.measure(m,60)==0&&writes(0x6b,0x0104)==stops+1);
    // A sleeping low-duty sensor must check snapshot age at its actual next wake.
    Senxx sleeper(bus,true);assert(sleeper.measure(m,2000)==-EAGAIN);
    k_msleep(30000);assert(sleeper.measure(m,2000)==0);
    vocOps=writes(0x6b,0x6181);(void)sleeper.measure(m,2000);
    assert(writes(0x6b,0x6181)==vocOps);
    epoch+=2001;k_msleep(2001000);assert(sleeper.measure(m,2000)==-EAGAIN&&writes(0x6b,0x6181)==vocOps);
    sen6Name="SEN63C";saved.erase(key);Senxx co2(bus,true);
    assert(co2.measure(m,60)==-EAGAIN);stops=writes(0x6b,0x0104);
    k_msleep(15000);m={};assert(co2.measure(m,60)==0&&!m.has_co2&&writes(0x6b,0x0104)==stops);
    k_msleep(SEN6X_CO2_WARMUP_MS-15000);m={};assert(co2.measure(m,60)==0&&m.has_co2&&writes(0x6b,0x0104)==stops+1);
    sen6Name="SEN66";epoch=0;saved.erase(key);
    puts("PASS SEN one-shot keeps measuring after empty VOC state, restores only at eligible wake and waits for CO2 warm-up");
}
static void lightningController(){
    auto f=std::make_unique<Fixture>();
    f->s.stop();auto config=f->s.config;config.setTxEnabled(true);
    assert(config.setDeviceRole(1,0)&&f->s.apply(config)&&f->s.start(false,0)&&f->s.armEndpoint(true));
    mt::ModuleController controller;mt::ModuleSettings settings;
    meshtastic_ModuleConfig cfg{};cfg.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;
    cfg.payload_variant.telemetry.environment_measurement_enabled=true;
    cfg.payload_variant.telemetry.environment_update_interval=3600;assert(settings.set(cfg));
    unsigned broadcasts=0,phoneOnly=0;
    auto poll=[&](uint64_t ms){
        f->channelNow=ms;
        controller.pollTimed(f->s,settings,true,ms,uint32_t(ms)^994433,1600000000,[&](const uint8_t *p,size_t n,uint32_t id,bool local){
            assert(id);meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Data_fields,&d));
            if(d.portnum!=meshtastic_PortNum_TELEMETRY_APP)return;
            meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
            if(t.which_variant!=meshtastic_Telemetry_environment_metrics_tag)return;
            assert(!d.request_id&&t.variant.environment_metrics.has_lightning_strike_count_1h);
            local?++phoneOnly:++broadcasts;
        });
    };
    auto clear=[&](){f->s.stop();assert(f->s.start(false,0)&&f->s.armEndpoint(true));};
    zt::telemetry::EnvironmentReadings sample{};
    sample.accessory.hasStrikes=sample.accessory.hasLastStrike=true;sample.accessory.strikes=1;sample.accessory.lastStrikeMs=1000;
    f->s.measuredEnvironment(sample);poll(0);assert(!broadcasts);
    uint8_t buf[239];size_t len=0;auto metric=zt::telemetry::toMeshtasticEnvironmentMetrics(f->s.environment());
    assert(mt::EnvironmentQuery::responseMetrics(metric,55,buf,sizeof(buf),len,1600000000));
    meshtastic_Data reply{};auto in=pb_istream_from_buffer(buf,len);assert(pb_decode(&in,meshtastic_Data_fields,&reply)&&reply.request_id==55);
    f->s.channelMeter().supported(false);poll(1000);assert(!broadcasts&&!f->s.endpointControls());
    f->s.channelMeter().supported(true);poll(1999);assert(!broadcasts);
    poll(2000);assert(broadcasts==1&&f->s.endpointControls()==1);poll(3000);assert(broadcasts==1);
    sample.accessory.lastStrikeMs=4000;++sample.accessory.strikes;f->s.measuredEnvironment(sample);
    poll(4000);assert(broadcasts==1&&f->s.endpointControls()==1);clear();poll(4999);assert(broadcasts==1);
    poll(5000);assert(broadcasts==2&&f->s.endpointControls()==1);clear();
    poll(55000);assert(broadcasts==2&&phoneOnly==1&&!f->s.endpointControls());
    sample.accessory.lastStrikeMs=60000;f->s.measuredEnvironment(sample);f->s.channelMeter().supported(false);poll(60000);
    f->s.channelMeter().supported(true);poll(360000);assert(broadcasts==2&&!f->s.endpointControls());
    sample.accessory.lastStrikeMs=360100;sample.accessory.hasLastStrike=false;f->s.measuredEnvironment(sample);poll(361000);
    assert(broadcasts==2&&!f->s.endpointControls()&&!f->radio.sends);
    puts("PASS AS3935 strike bypasses periodic interval once after admission, retries denied channel/full queue, preserves direct response and independent phone cadence, expires at five minutes");
}
struct AccessoryHost:Host {
    using Host::Host;
    int sensorAdmin(const meshtastic_SensorConfig &c,uint32_t s,uint32_t i)override{return submit(c,s,i);}
    int scaleAdmin(uint32_t kg,uint32_t s,uint32_t i)override{return submitScale(kg,s,i);}
    bool sensorAdminResult(uint32_t s,uint32_t &i,int &r)override{return take(s,i,r);}
};
static void packet(mt::Companion &c,meshtastic_AdminMessage &a,unsigned id,bool malformed=false){
    meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=t.packet;
    p.to=OURS;p.id=id;p.pki_encrypted=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
    auto w=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&w,meshtastic_AdminMessage_fields,&a));
    p.decoded.payload.size=w.bytes_written;if(malformed){p.decoded.payload.bytes[p.decoded.payload.size++]=8;p.decoded.payload.bytes[p.decoded.payload.size++]=0;}
    input(c,t,99);
}
static void work(){try{mockKernel::accessoryThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob&){}}
int main(){
    protocols();senMaintenance();lightningController();
    Port rainPort(0x1d);Rainfall rain(rainPort);float hour=-1,day=-1;
    assert(!rain.sample(hour,day)&&!writes(0x1d,0x26));
    const uint8_t id[]={0xc0,0,0x43,0x73};memcpy(registers[0x1d],id,4);
    failure=24;assert(!rain.sample(hour,day)&&hour==-1&&day==-1);failure=0;
    assert(rain.sample(hour,day)&&hour==float(0x10203/10000.0)&&day==float(0x01020304/10000.0));
    puts("PASS SEN0575 identity, hourly windows, little-endian totals, conversion wait, partial failure and recovery");
    Port lightPort(0x53);Ltr390 light(lightPort);donorLight::LTR390UVSensor oracle;
    float lux=0,uv=0;
    assert(!light.sample(lux,uv)); // wrong part ID
    auto *reg=registers[0x53];reg[6]=0xb2;reg[7]=0;failResetRead=true;assert(!light.sample(lux,uv)&&busRecoveries==1&&!lightBusStuck);
    reg[7]=8;
    for(unsigned i=0;i<24;i++){
        auto &stock=oracle.ltr390uv;
        const unsigned raw=(i*84557u)&0xfffff,at=stock.mode?0x10:0x0d;
        reg[at]=raw;reg[at+1]=raw>>8;reg[at+2]=raw>>16;
        if(stock.mode)stock.uv=raw;else stock.als=raw;
        meshtastic_Telemetry donor{};
        assert(oracle.getMetrics(&donor)&&light.sample(lux,uv));
        assert(donor.variant.environment_metrics.lux==lux&&donor.variant.environment_metrics.uv_lux==uv);
        assert(((reg[0]>>3)&1)==stock.mode&&(reg[5]&7)==stock.gain);
    }
    failure=0x535;assert(!light.sample(lux,uv));failure=0;
    assert(reg[0]&2);
    failure=0x530;light.stop();assert(reg[0]&2);
    failure=0;light.stop();assert(!(reg[0]&2));
    assert(light.sample(lux,uv));light.stop();assert(!(reg[0]&2));
    puts("PASS LTR390 paired visible and UV measurements, pinned alternating donor, gains, unavailable data, failed switch and idle");

    Port range(0x57), radiation(0x66);
    float reading=0;
    donorRange::Wire wire;
    donorRange::RCWL9620Sensor stockRange{&wire};
    donorRange::CGRadSensSensor stockRadiation{&wire};
    for(uint32_t raw:{0u,1u,0xffffu,0x10000u,4499999u,4500000u,4500001u,0xffffffu}){
        wire.data[0]=raw>>16;wire.data[1]=raw>>8;wire.data[2]=raw;
        memcpy(registers[0x57],wire.data,3);memcpy(registers[0x66]+6,wire.data,3);
        assert(ultrasonicRange(range,reading)&&reading==stockRange.getDistance());
        assert(staticRadiation(radiation,reading)&&reading==stockRadiation.getStaticRadiation());
    }
    memset(registers[0x57],0,3);
    assert(ultrasonicRange(range,reading)&&reading==0);
    memset(registers[0x57],0xff,3);
    assert(ultrasonicRange(range,reading)&&reading==4500);
    for(uint16_t fault:{0x571,0x572}){
        failure=fault;assert(!ultrasonicRange(range,reading)&&reading==4500);
    }
    for(uint16_t fault:{0x660,0x661}){
        failure=fault;assert(!staticRadiation(radiation,reading)&&reading==4500);
    }
    failure=0;
    registers[0x66][6]=1;registers[0x66][7]=2;registers[0x66][8]=3;
    registers[0x57][0]=1;registers[0x57][1]=2;registers[0x57][2]=3;

    auto f=std::make_unique<Fixture>();AccessoryHost h(f->s);mt::Companion c(f->s,h,81,f->workspace,0);
    meshtastic_ToRadio init{};init.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;init.want_config_id=17;
    input(c,init,4);for(int i=0;i<50;i++)c.poll(mt::Companion::WorkspaceProof{true});h.out.clear();
    meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_sensor_config_tag;
    a.sensor_config.has_as3935_config=true;a.sensor_config.as3935_config.has_set_tuning_cap_pf=true;a.sensor_config.as3935_config.set_tuning_cap_pf=32;
    packet(c,a,201,true);assert(results(h,201,meshtastic_Routing_Error_BAD_REQUEST)==1);
    packet(c,a,200);packet(c,a,200);assert(!results(h,200));work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,200,0)==1);
    const auto n=trace.size();packet(c,a,200);work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,200,0)==2&&n==trace.size());
    a.which_payload_variant=meshtastic_AdminMessage_set_scale_tag;a.set_scale=0;
    packet(c,a,200);assert(results(h,200,meshtastic_Routing_Error_BAD_REQUEST)==1);
    packet(c,a,202);work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,202,0)==1);
    enableAir(true,60);enableEnvironment(true);mockKernel::timerTicks=40;work();
    meshtastic_AirQualityMetrics m{};assert(air(k_uptime_get(),m)&&m.co2==507&&m.has_pm40_standard);
    auto env=environment(k_uptime_get());assert(env.hasStrikes&&env.hasWeight);
    zt::telemetry::EnvironmentReadings sample{};sample.accessory=env;
    meshtastic_Telemetry announcement{};assert(mt::stock::telemetry::environmentReport(announcement,100,sample));
    const auto &metrics=announcement.variant.environment_metrics;
    assert(metrics.has_lightning_strike_count_1h&&metrics.has_weight);
    uint8_t payload[239];size_t size=0;
    assert(mt::EnvironmentQuery::responseMetrics(metrics,77,payload,sizeof(payload),size,100));
    meshtastic_Data data{};auto stream=pb_istream_from_buffer(payload,size);assert(pb_decode(&stream,meshtastic_Data_fields,&data));
    meshtastic_Telemetry reply{};stream=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(data.request_id==77&&pb_decode(&stream,meshtastic_Telemetry_fields,&reply));
    assert(reply.variant.environment_metrics.has_weight&&reply.variant.environment_metrics.has_lightning_strike_count_1h);
    assert(metrics.has_wind_speed&&metrics.wind_speed==7.25f&&metrics.has_wind_direction&&metrics.wind_direction==157);
    assert(metrics.temperature==-7.75f&&metrics.relative_humidity==81.25f&&metrics.barometric_pressure==1003.5f);
    donorWeather::DFRobotLarkSensor station;meshtastic_Telemetry donorWeatherReport{};
    assert(station.getMetrics(&donorWeatherReport));const auto &stockWeather=donorWeatherReport.variant.environment_metrics;
    assert(stockWeather.wind_speed==metrics.wind_speed&&stockWeather.wind_direction==metrics.wind_direction&&
           stockWeather.relative_humidity==metrics.relative_humidity&&stockWeather.barometric_pressure==metrics.barometric_pressure&&
           stockWeather.temperature==metrics.temperature);
    assert(reply.variant.environment_metrics.has_wind_speed&&reply.variant.environment_metrics.wind_direction==157);
    assert(metrics.has_rainfall_1h&&metrics.has_rainfall_24h&&metrics.rainfall_1h==hour&&metrics.rainfall_24h==day);
    assert(reply.variant.environment_metrics.has_rainfall_24h&&reply.variant.environment_metrics.rainfall_24h==day);
    assert(metrics.has_lux&&metrics.has_uv_lux&&metrics.uv_lux>0&&metrics.lux>0);
    assert(reply.variant.environment_metrics.has_uv_lux&&reply.variant.environment_metrics.uv_lux==metrics.uv_lux);
    assert(metrics.has_radiation&&metrics.radiation==6605.1f&&metrics.has_distance&&metrics.distance==66.051f);
    assert(reply.variant.environment_metrics.radiation==metrics.radiation&&reply.variant.environment_metrics.distance==metrics.distance);
    puts("PASS RadSens static intensity and RCWL9620 distance, bus errors, range saturation, worker report and phone query");
    // A failed fifth measurement cannot withdraw the already measured Dropzone quartet.
    larkHumidityFails=true;
    requestWeather();mockKernel::timerTicks=12;work();
    Environment sharedDrop;
    assert(dropzoneWeather(sharedDrop)&&sharedDrop.hasWeather&&sharedDrop.wind==7.25f&&sharedDrop.bearing==157);
    zt::telemetry::EnvironmentReadings partial;partial.accessory=environment(k_uptime_get());
    assert(partial.accessory.hasWeather&&!partial.accessory.hasWeatherHumidity);
    meshtastic_Telemetry partialReport{};
    assert(mt::stock::telemetry::environmentReport(partialReport,0,partial));
    const auto &partialMetrics=partialReport.variant.environment_metrics;
    assert(partialMetrics.has_wind_speed&&partialMetrics.has_wind_direction&&!partialMetrics.has_relative_humidity);
    auto queryWind=[&](const meshtastic_EnvironmentMetrics &v, bool speed,bool bearing){
        assert(mt::EnvironmentQuery::responseMetrics(v,307,payload,sizeof(payload),size));
        meshtastic_Data wrapped{};auto input=pb_istream_from_buffer(payload,size);
        assert(pb_decode(&input,meshtastic_Data_fields,&wrapped)&&wrapped.request_id==307);
        meshtastic_Telemetry decoded{};input=pb_istream_from_buffer(wrapped.payload.bytes,wrapped.payload.size);
        assert(pb_decode(&input,meshtastic_Telemetry_fields,&decoded));
        const auto &m=decoded.variant.environment_metrics;
        assert(m.has_wind_speed==speed&&m.has_wind_direction==bearing);
        if(speed)assert(m.wind_speed==v.wind_speed);
        if(bearing)assert(m.wind_direction==v.wind_direction);
        assert(!m.has_relative_humidity);
    };
    queryWind(partialMetrics,true,true);
    meshtastic_EnvironmentMetrics wind{};wind.has_wind_speed=true;wind.wind_speed=0;
    queryWind(wind,true,false);wind.has_wind_speed=false;wind.has_wind_direction=true;wind.wind_direction=0;
    queryWind(wind,false,true);wind.has_wind_speed=true;wind.wind_speed=-1;queryWind(wind,false,true);
    wind.wind_direction=360;assert(!mt::EnvironmentQuery::responseMetrics(wind,307,payload,sizeof(payload),size));
    assert(!air(k_uptime_get()+90001,m));enableAir(false,60);enableEnvironment(false);
    assert(!air(k_uptime_get(),m)&&!environment(k_uptime_get()).hasStrikes);
    Environment drop{};const unsigned beforeHumidity=larkHumidityRequests;
    requestWeather();assert(!dropzoneWeather(drop));mockKernel::timerTicks=15;work();
    assert(dropzoneWeather(drop)&&drop.hasWeather&&drop.wind==7.25f&&!environment(k_uptime_get()).hasWeather);
    assert(larkHumidityRequests==beforeHumidity);larkHumidityFails=false;
    larkFails=true;drop={};requestWeather();mockKernel::timerTicks=2;work();
    assert(dropzoneWeather(drop)&&!drop.hasWeather);larkFails=false;
    puts("PASS shared Lark worker, four-field Dropzone without humidity, independent wind in phone queries, and failed reads");
    puts("PASS canonical USB calibration, delayed Routing result and collision/retry protection; optional worker sensor acquisition, fresh caches and enable gates");
}
