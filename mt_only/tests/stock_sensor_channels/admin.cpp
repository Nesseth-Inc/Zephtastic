// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "stock/telemetry/Scd4xAdmin.h"
#include "stock/telemetry/ZephyrShtAdmin.h"
#include "stock/telemetry/ZephyrScd4xAdmin.h"
#include <zephyr/kernel.h>
extern "C" {
#include <zephyr/drivers/sensor/scd4x.h>
}
#include <map>
#define LOG_DEBUG(...)
#define LOG_INFO(...)
#define LOG_ERROR(...)
#include "CO2Sensor.h"
extern std::map<int,double> attrs;
extern int shtAccuracyLevel,shtError;
extern bool lowPower,baselineError;
extern int writes,modeChanges,calibrationSampleError;
extern uint16_t baselinePpm;
extern bool frcError;extern int forced,resets,persists;
namespace board=mt::stock::telemetry::scd4x;
void work(){
    try{mockKernel::scd4xAdminThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob&){}
}
struct CalibrationTrace : CO2CalibrationSensor {
    std::string trace;
    bool failure=false;
    bool co2PerformFRC(uint32_t n)override{trace+="F"+std::to_string(n);return !failure;}
    bool co2GetASC(bool &v)override{trace+="?";v=true;return true;}
    bool co2SetASC(bool v)override{trace+=v?"Y":"N";return true;}
    bool co2SetASCBaseline(uint32_t n)override{trace+="B"+std::to_string(n);return !failure;}
    bool co2SetAltitude(uint32_t m)override{trace+="A"+std::to_string(m);return true;}
    bool co2SetAmbientPressure(uint32_t p)override{trace+="P"+std::to_string(p);return true;}
    bool co2FactoryReset()override{trace+="R";return true;}
    bool reference(const meshtastic_SCD4X_config &q){
        Co2AdminRequest x{q.has_factory_reset,q.has_set_asc,q.set_asc,q.has_set_target_co2_conc,q.set_target_co2_conc,
                          q.has_set_altitude,q.set_altitude,q.has_set_ambient_pressure,q.set_ambient_pressure};
        return handleCo2AdminRequest(x,"scd41");
    }
    int setAsc(bool v){return co2SetASC(v)?0:-EIO;}
    int getAsc(bool &v){return co2GetASC(v)?0:-EIO;}
    int forcedCalibration(uint16_t n){return co2PerformFRC(n)?0:-EIO;}
    int altitude(uint32_t m){return co2SetAltitude(m)?0:-EIO;}
    int pressure(uint32_t p){return co2SetAmbientPressure(p)?0:-EIO;}
    int baseline(uint16_t n){return co2SetASCBaseline(n)?0:-EIO;}
    int mode(bool){return 0;}
    int temperature(float){return 0;}
    int factoryReset(){return co2FactoryReset()?0:-EIO;}
};
struct SensorHost : Host {
    using Host::Host;
    int shtAccuracy(const meshtastic_SensorConfig &cfg)override{return mt::stock::telemetry::setShtAccuracy(cfg);}
    int oneWireAdmin(unsigned channel)override{return mt::stock::telemetry::oneWire::select(channel);}
    int sensorAdmin(const meshtastic_SensorConfig &c,uint32_t session,uint32_t id)override{return board::submit(c,session,id);}
    bool sensorAdminResult(uint32_t session,uint32_t &id,int &r)override{return board::take(session,id,r);}
};
static void admin(mt::Companion &client,const meshtastic_SensorConfig &cfg,uint32_t id,bool malformed=false){
    meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_sensor_config_tag;a.sensor_config=cfg;
    meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=t.packet;p.to=OURS;p.id=id;p.pki_encrypted=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
    auto w=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&w,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=w.bytes_written;
    if(malformed){p.decoded.payload.bytes[p.decoded.payload.size++]=8;p.decoded.payload.bytes[p.decoded.payload.size++]=0;}
    input(client,t,99);
}
void adminCases(){
    for(unsigned mask=0;mask<64;mask++)for(bool fail:{false,true}){
        meshtastic_SCD4X_config q{};q.has_factory_reset=mask&1;q.has_set_asc=mask&2;q.set_asc=mask&4;
        q.has_set_target_co2_conc=q.has_set_asc&&(!q.set_asc||(mask&32));q.set_target_co2_conc=435;
        q.has_set_altitude=mask&8;q.set_altitude=123;q.has_set_ambient_pressure=mask&16;q.set_ambient_pressure=98700;
        CalibrationTrace ref,actual;ref.failure=actual.failure=fail;
        const bool expected=ref.reference(q);
        assert((mt::stock::telemetry::applyScd4x(actual,q)==0)==expected&&actual.trace==ref.trace);
    }
    auto f=std::make_unique<Fixture>();SensorHost h(f->s);mt::Companion c(f->s,h,81,f->workspace,0);
    meshtastic_ToRadio init{};init.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;init.want_config_id=17;
    input(c,init,4);for(int i=0;i<50;i++)c.poll(mt::Companion::WorkspaceProof{true});h.out.clear();
    meshtastic_SensorConfig cfg{};cfg.has_scd4x_config=true;auto &q=cfg.scd4x_config;
    q.has_set_asc=true;q.set_asc=false;q.has_set_target_co2_conc=true;q.set_target_co2_conc=435;
    attrs[SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE]=1;
    admin(c,cfg,201,true);assert(results(h,201,meshtastic_Routing_Error_BAD_REQUEST)==1);assert(!forced);
    admin(c,cfg,200);admin(c,cfg,200);assert(!results(h,200));work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,200,0)==1&&forced==1);
    admin(c,cfg,200);work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,200,0)==2&&forced==1);
    q.set_target_co2_conc=436;admin(c,cfg,200);assert(results(h,200,meshtastic_Routing_Error_BAD_REQUEST)==1&&forced==1);
    q.set_target_co2_conc=435;frcError=true;attrs[SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE]=1;
    admin(c,cfg,202);work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,202,0)==0&&results(h,202)==1&&forced==2);
    assert(attrs[SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE]==1);frcError=false;
    q={};q.has_set_ambient_pressure=true;q.set_ambient_pressure=101325;q.has_set_temperature=true;q.set_temperature=21.25;
    admin(c,cfg,203);work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,203,0)==1&&attrs[SENSOR_ATTR_SCD4X_AMBIENT_PRESSURE]==1013&&attrs[SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET]==2);
    q={};q.has_factory_reset=q.factory_reset=true;admin(c,cfg,204);work();c.resetStream();
    input(c,init,700);for(int i=0;i<50;i++)c.poll(mt::Companion::WorkspaceProof{true});
    assert(resets==1&&!results(h,204));
    q={};q.has_set_power_mode=q.set_power_mode=true;admin(c,cfg,205);assert(!results(h,205));work();
    c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,205,0)==1&&lowPower&&modeChanges==1);
    q={};q.has_set_target_co2_conc=true;q.set_target_co2_conc=480;q.has_set_asc=q.set_asc=true;
    admin(c,cfg,208);work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,208,0)==1&&writes==1&&baselinePpm==480);
    admin(c,cfg,208);work();c.poll(mt::Companion::WorkspaceProof{true});assert(results(h,208,0)==2&&writes==1);
    q.set_target_co2_conc=70000;admin(c,cfg,209);assert(results(h,209,meshtastic_Routing_Error_BAD_REQUEST)==1&&writes==1);
    q.set_target_co2_conc=470;baselineError=true;admin(c,cfg,210);work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,210)==1&&!results(h,210,0)&&baselinePpm==480&&writes==2);
    admin(c,cfg,210);work();c.poll(mt::Companion::WorkspaceProof{true});assert(writes==2);
    baselineError=false;admin(c,cfg,211);work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,211,0)==1&&baselinePpm==470&&writes==3);
    q={};q.has_set_temperature=true;q.set_temperature=23.5;q.has_set_power_mode=true;q.set_power_mode=false;
    calibrationSampleError=-EAGAIN;admin(c,cfg,212);work();c.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,212)==1&&!results(h,212,0)&&!lowPower&&modeChanges==2);calibrationSampleError=0;
    cfg={};cfg.has_ds248x_config=true;cfg.ds248x_config.has_main_temperature_channel=true;
    cfg.ds248x_config.main_temperature_channel=6;admin(c,cfg,206);assert(results(h,206,0)==1);
    cfg.ds248x_config.main_temperature_channel=8;admin(c,cfg,207);assert(!results(h,207,0)&&results(h,207)==1);
    cfg={};cfg.has_shtxx_config=true;cfg.shtxx_config.has_set_accuracy=true;
    for(unsigned level=0;level<=2;level++){
        cfg.shtxx_config.set_accuracy=level;admin(c,cfg,220+level);
        assert(results(h,220+level,0)==1&&shtAccuracyLevel==int(level));
    }
    cfg.shtxx_config.set_accuracy=3;admin(c,cfg,224);
    assert(results(h,224,meshtastic_Routing_Error_BAD_REQUEST)==1&&shtAccuracyLevel==2);
    cfg.shtxx_config.set_accuracy=0;cfg.has_scd4x_config=true;admin(c,cfg,225);
    assert(results(h,225,meshtastic_Routing_Error_BAD_REQUEST)==1&&shtAccuracyLevel==2);
    cfg.has_scd4x_config=false;shtError=-EIO;admin(c,cfg,226);
    assert(results(h,226)==1&&!results(h,226,0)&&shtAccuracyLevel==2);shtError=0;
    puts("PASS canonical SHTxx accuracy on USB, all three levels, mixed/invalid requests and driver failure");
    puts("PASS 128 pinned CO2 calibration traces, canonical USB Admin, delayed driver outcome, collision and retry dedup, rollback, session isolation");
}
