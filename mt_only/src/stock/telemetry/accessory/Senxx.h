// SPDX-License-Identifier: GPL-3.0-only
// SENXXSensor.cpp, Meshtastic 8e6a88d. The product identifier selects the
// model-specific output length and opcode before any calibration is attempted.
#pragma once
#include "SenHistory.h"
#include <meshtastic/admin.pb.h>
#include <meshtastic/telemetry.pb.h>
namespace mt::stock::telemetry::accessory {
class Senxx {
    struct Model {const char *name;uint16_t read;bool rht,voc,nox,hcho,co2;};
    static constexpr Model models[]={
        {"SEN50",0x03c4,0,0,0,0,0},{"SEN54",0x03c4,1,1,0,0,0},{"SEN55",0x03c4,1,1,1,0,0},
        {"SEN62",0x04a3,1,0,0,0,0},{"SEN63C",0x0471,1,0,0,0,1},{"SEN65",0x0446,1,1,1,0,0},
        {"SEN66",0x0300,1,1,1,0,1},{"SEN68",0x0467,1,1,1,1,0},{"SEN69C",0x04b5,1,1,1,1,1}};
    Bus &bus;const bool six;SenHistory history;
    const Model *model=nullptr;
    enum class State {Idle,Gas,Active,Cleaning};
    State state=State::Idle;
    bool oneShot=true,haveTemp=false;
    float temperature=0;
    uint64_t started=0,nextWake=0,cleanEnd=0,tempAt=0,gasSince=0,nextSave=0,cleanRetry=0,restoreRetry=0;
    bool gasRunning=false,warmRestored=false,vocAvailable=false,undatedClean=false;
    const char *key()const{return six?"mtxsensor/sen6":"mtxsensor/sen5";}
    int probe(){
        if(model)return 0;
        uint16_t w[16];int e=bus.words(0xd014,w,16,50);if(e)return e;
        char name[33]={};for(size_t i=0;i<16;i++){name[2*i]=char(w[i]>>8);name[2*i+1]=char(w[i]);}
        uint8_t stored=1;e=bus.load(key(),&stored,sizeof(stored));
        if(e&&e!=-ENOENT)return e;
        if(stored>1)return -EINVAL;
        for(const auto &m:models)if(!strcmp(name,m.name)&&six==(name[3]=='6')){
            e=history.identify(uint32_t(&m-models)+1);
            if(e)return e;
            model=&m;oneShot=stored;return 0;
        }
        return -ENODEV;
    }
    int start(){
        int e=bus.command(0x0021,50);
        if(!e){
            state=State::Active;started=bus.now();
            if(!gasRunning){gasRunning=true;gasSince=started;vocAvailable=warmRestored;}
        }
        return e;
    }
    bool vocStable()const{return gasRunning&&(warmRestored||(bus.now()-gasSince)/1000>3600);}
    int maintain(){
        const uint32_t utc=bus.utc();
        (void)history.flush();
        if(undatedClean&&SenHistory::credible(utc)){history.cleaned(utc);undatedClean=false;}
        if(history.due(utc)&&bus.now()>=cleanRetry){
            // A failed command can be retried while ordinary telemetry continues.
            cleanRetry=bus.now()+60000;
            return clean();
        }
        if(model->voc&&(state==State::Active||bus.now()>=nextWake)&&history.canRestore(utc)&&bus.now()>=restoreRetry){
            restoreRetry=bus.now()+1000;
            const bool active=state==State::Active;
            // A controller reboot can leave an independently powered sensor measuring.
            const int stop=idle(false);
            if(stop)return stop;
            int e=history.restore();
            warmRestored=!e;vocAvailable=!e;
            if(active){int resumed=start();if(!e)e=resumed;}
            return e;
        }
        return 0;
    }
    int idle(bool allowGas){
        const bool gas=allowGas&&!six&&model->voc;
        int e=bus.command(gas?0x0037:0x0104,six?1400:200);
        if(!e){
            state=gas?State::Gas:State::Idle;
            if(!gas){gasRunning=false;warmRestored=false;history.markStopped();}
        }
        return e;
    }
    int mode(bool one){
        if(oneShot==one)return 0;
        const uint8_t value=one;
        int e=bus.save(key(),&value,1);
        if(!e){oneShot=one;nextWake=0;}
        return e;
    }
    int clean(){
        if(state==State::Cleaning)return -EBUSY;
        int e=start();
        if(!e)e=bus.command(0x5607,20);
        if(!e){state=State::Cleaning;cleanEnd=bus.now()+10500;}
        return e;
    }
    int finishCleaning(){
        if(state!=State::Cleaning)return 0;
        if(bus.now()<cleanEnd)return -EBUSY;
        const uint32_t utc=bus.utc();
        if(SenHistory::credible(utc))history.cleaned(utc);else undatedClean=true;
        state=State::Active;started=bus.now();
        if(oneShot)return idle(true);
        return 0;
    }
    int co2(const meshtastic_SEN6X_config &q){
        if(q.has_factory_reset)return bus.command(0x6754,1400);
        int e=0;
        if(q.has_set_asc){
            if(q.set_asc)e=bus.word(0x6711,1,20);
            else{
                uint16_t previous=0,correction=0;
                e=bus.words(0x6711,&previous,1,20);
                if(!e)e=bus.word(0x6711,0,20);
                if(!e){
                    e=bus.word(0x6707,q.set_target_co2_conc,500);
                    if(!e){uint8_t rx[3];e=bus.read(rx,3);
                        if(!e){correction=uint16_t(rx[0])<<8|rx[1];if(Bus::crc(rx)!=rx[2]||correction==65535)e=-EIO;}}
                    if(e)(void)bus.word(0x6711,previous?1:0,20);
                }
            }
        }
        if(!e&&q.has_set_altitude)e=bus.word(0x6736,q.set_altitude,20);
        else if(!e&&q.has_set_ambient_pressure)e=bus.word(0x6720,q.set_ambient_pressure/100,20);
        return e;
    }
public:
    explicit Senxx(Bus &b,bool familySix):bus(b),six(familySix),history(b,familySix?"mtxsensor/sen6hist":"mtxsensor/sen5hist"){}
    int apply(const meshtastic_SensorConfig &s){
        if(six!=s.has_sen6x_config||six==s.has_sen5x_config)return -EINVAL;
        int e=probe();if(e)return e;
        const auto &q=s.sen6x_config;const auto &p=s.sen5x_config;
        const bool setMode=six?q.has_set_one_shot_mode:p.has_set_one_shot_mode;
        const bool cleaning=six?q.has_start_fan_cleaning&&q.start_fan_cleaning:p.has_start_fan_cleaning&&p.start_fan_cleaning;
        const bool carbon=six&&(q.has_factory_reset||q.has_set_asc||q.has_set_altitude||q.has_set_ambient_pressure);
        const bool temp=six?q.has_set_temperature:p.has_set_temperature;
        const float reference=six?q.set_temperature:p.set_temperature;
        if(temp&&!model->rht)return -ENOTSUP;
        if(!setMode&&!cleaning&&!temp&&!carbon)return -EINVAL;
        if(cleaning&&(carbon||temp))return -EINVAL;
        if(carbon&&!model->co2)return -ENOTSUP;
        if(six&&((q.has_factory_reset&&!q.factory_reset)||(q.has_set_altitude&&q.set_altitude>3000)||
           (q.has_set_target_co2_conc&&(!q.has_set_asc||q.set_target_co2_conc==0||q.set_target_co2_conc>65535))||
           (q.has_set_asc&&!q.set_asc&&!q.has_set_target_co2_conc)||
           (!q.has_set_altitude&&q.has_set_ambient_pressure&&(q.set_ambient_pressure<70000||q.set_ambient_pressure>120000))))return -EINVAL;
        uint16_t offsets[4]={};
        if(temp){
            if(!validTemp(reference))return -EINVAL;
            if(!haveTemp||bus.now()<tempAt||bus.now()-tempAt>90000)return -EAGAIN;
            float diff=(temperature-reference)*200;
            if(!(diff>=-32768&&diff<=32767))return -ERANGE;
            offsets[0]=uint16_t(int16_t(diff));
        }
        e=finishCleaning();if(e)return e;
        if(setMode){e=mode(six?q.set_one_shot_mode:p.set_one_shot_mode);if(e)return e;}
        if(cleaning)return clean();
        if(!temp&&!carbon)return 0;
        const bool wasActive=state==State::Active;
        if(wasActive){e=idle(false);if(e)return e;}
        if(temp)e=bus.command(0x60b2,20,offsets,six?4:3);
        if(!e&&carbon)e=co2(q);
        if(wasActive){int restore=start();if(!e)e=restore;}
        return e;
    }
    int measure(meshtastic_AirQualityMetrics &m,uint32_t intervalSeconds){
        int e=probe();if(e)return e;
        e=finishCleaning();if(e)return e;
        e=maintain();if(e)return e;
        if(state==State::Cleaning)return -EBUSY;
        if(state!=State::Active){if(bus.now()<nextWake)return -EAGAIN;e=start();return e?e:-EAGAIN;}
        const uint64_t elapsed=bus.now()-started;
        if(elapsed<15000)return -EAGAIN;
        uint16_t ready=0,w[10]={},pn[10]={};
        e=bus.words(0x0202,&ready,1,20);if(e||!(ready&0xff))return e?e:-EAGAIN;
        const size_t length=six?4+2*model->rht+model->voc+model->nox+model->hcho+model->co2:8;
        e=bus.words(model->read,w,length,20);if(e)return e;
        e=bus.words(six?0x0316:0x0413,pn,six?5:10,20);if(e)return e;
        const auto *counts=pn+(six?0:4);
        if(elapsed<30000&&(counts[3]==65535||counts[3]<1000))return -EAGAIN;
        uint32_t *mass[]={&m.pm10_standard,&m.pm25_standard,&m.pm40_standard,&m.pm100_standard};
        bool *hasMass[]={&m.has_pm10_standard,&m.has_pm25_standard,&m.has_pm40_standard,&m.has_pm100_standard};
        bool valid=false;
        for(unsigned i=0;i<4;i++)if(w[i]!=65535){*hasMass[i]=true;*mass[i]=w[i]/10;valid=true;}
        uint32_t *out[]={&m.particles_05um,&m.particles_10um,&m.particles_25um,&m.particles_40um,&m.particles_100um};
        bool *present[]={&m.has_particles_05um,&m.has_particles_10um,&m.has_particles_25um,&m.has_particles_40um,&m.has_particles_100um};
        for(unsigned i=0;i<5;i++)if(counts[i]!=65535&&(!i||counts[i-1]==65535||counts[i]>=counts[i-1])){
            *present[i]=true;*out[i]=10u*(counts[i]-(i&&counts[i-1]!=65535?counts[i-1]:0));valid=true;
        }
        if(!six&&pn[9]!=65535){m.has_particles_tps=true;m.particles_tps=pn[9]/1000.f;valid=true;}
        size_t i=4;
        if(model->rht){
            if(w[i]!=0x7fff){float rh=int16_t(w[i])/100.f;if(rh>=0&&rh<=100){m.has_pm_humidity=true;m.pm_humidity=rh;valid=true;}}++i;
            if(w[i]!=0x7fff){temperature=int16_t(w[i])/200.f;tempAt=bus.now();haveTemp=true;m.has_pm_temperature=true;m.pm_temperature=temperature;valid=true;}++i;
        }
        if(model->voc){if(w[i]!=0x7fff&&int16_t(w[i])>=0){m.has_pm_voc_idx=true;m.pm_voc_idx=w[i]/10.f;valid=true;}++i;}
        if(model->nox){if(w[i]!=0x7fff&&int16_t(w[i])>=0){m.has_pm_nox_idx=true;m.pm_nox_idx=w[i]/10.f;valid=true;}++i;}
        if(model->hcho){if(w[i]!=65535){m.has_form_formaldehyde=true;m.form_formaldehyde=w[i]/10.f;valid=true;}++i;}
        if(model->co2&&w[i]!=65535&&elapsed>=24000){m.has_co2=true;m.co2=w[i];valid=true;}
        if(six){uint16_t status[2];if(!bus.words(0xd206,status,2,20)){m.has_pm_status_flags=true;m.pm_status_flags=uint32_t(status[0])<<16|status[1];}}
        if(valid&&model->voc){
            // Preserve the gas engine while it learns the local air baseline.
            if(!vocStable())return 0;
            if(bus.now()>=nextSave){
                int saved=history.capture(bus.utc());
                nextSave=bus.now()+(saved?1000:300000);
                vocAvailable=!saved;
            }
            if(oneShot&&!vocAvailable)return 0;
        }
        if(valid&&oneShot&&model->co2&&elapsed<24000)return 0;
        if(valid&&oneShot){e=idle(true);if(e)return e;nextWake=started+uint64_t(intervalSeconds)*1000;}
        return valid?0:-EAGAIN;
    }
    void stop(){
        if(state==State::Active||state==State::Gas){
            if(model&&model->voc&&vocStable()&&bus.now()>=nextSave){
                int e=history.capture(bus.utc());nextSave=bus.now()+(e?1000:300000);
            }
            (void)idle(false);nextWake=0;
        }
    }
    void maintenance(){
        if(model)(void)history.flush();
        if(state==State::Cleaning)(void)finishCleaning();
    }
};
}
