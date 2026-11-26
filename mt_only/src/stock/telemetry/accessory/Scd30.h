// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic SCD30Sensor.cpp at 8e6a88d and Sensirion I2C SCD30 1.1.1.
#pragma once
#include "SensorBus.h"
#include <meshtastic/admin.pb.h>
#include <meshtastic/telemetry.pb.h>
namespace mt::stock::telemetry::accessory {
class Scd30 {
    Bus &bus;
    bool running=false;
    int start(){int e=bus.word(0x0010,0,10);running=!e;return e;}
    int setAsc(bool enable){
        int e=bus.word(0x5306,enable,10);uint16_t actual=0;
        if(!e)e=bus.words(0x5306,&actual,1,10);
        return e?e:(actual==enable?0:-EIO);
    }
    static float decodeFloat(const uint16_t *p){
        uint32_t bits=uint32_t(p[0])<<16|p[1];float value;memcpy(&value,&bits,4);return value;
    }
public:
    explicit Scd30(Bus &b):bus(b){}
    void stop(){if(running&&!bus.command(0x0104,10))running=false;}
    int measure(meshtastic_AirQualityMetrics &m){
        if(!running){int e=start();return e?e:-EAGAIN;}
        uint16_t ready=0,w[6];int e=bus.words(0x0202,&ready,1,10);
        if(e||!ready)return e?e:-EAGAIN;
        e=bus.words(0x0300,w,6,10);if(e)return e;
        const float ppm=decodeFloat(w),t=decodeFloat(w+2),rh=decodeFloat(w+4);
        if(!(ppm>=1&&ppm<=65535&&validTemp(t)&&rh>=0&&rh<=100))return -ERANGE;
        m.has_co2=m.has_co2_temperature=m.has_co2_humidity=true;
        m.co2=uint32_t(ppm);m.co2_temperature=t;m.co2_humidity=rh;return 0;
    }
    int apply(const meshtastic_SCD30_config &q){
        if(q.has_soft_reset){
            if(!q.soft_reset)return -EINVAL;
            int e=bus.command(0xd304,2000);if(e)return e;running=false;return start();
        }
        if(!q.has_set_asc&&!q.has_set_altitude&&!q.has_set_temperature&&!q.has_set_measurement_interval)return -EINVAL;
        if(q.has_set_target_co2_conc&&(!q.has_set_asc||!q.set_target_co2_conc||q.set_target_co2_conc>65535))return -EINVAL;
        if(q.has_set_asc&&!q.set_asc&&!q.has_set_target_co2_conc)return -EINVAL;
        if((q.has_set_altitude&&q.set_altitude>65535)||(q.has_set_temperature&&!validTemp(q.set_temperature))||
           (q.has_set_measurement_interval&&(q.set_measurement_interval<2||q.set_measurement_interval>1800)))return -EINVAL;
        int e=running?0:start();
        if(!e&&q.has_set_asc){
            if(q.set_asc)e=setAsc(true);
            else{
                uint16_t previous=0;e=bus.words(0x5306,&previous,1,10);
                if(!e)e=setAsc(false);
                if(!e){e=bus.word(0x5204,q.set_target_co2_conc,10);if(e)(void)setAsc(previous!=0);}
            }
        }
        if(!e&&q.has_set_altitude)e=bus.word(0x5102,q.set_altitude,10);
        if(!e&&q.has_set_temperature){
            uint16_t offset=0;
            if(q.set_temperature!=100){
                meshtastic_AirQualityMetrics m{};e=measure(m);if(e)return e;
                const float v=(m.co2_temperature-q.set_temperature)*100;
                if(!(v>=0&&v<=65535))return -ERANGE;
                offset=uint16_t(v);
            }
            e=bus.word(0x5403,offset,10);
        }
        if(!e&&q.has_set_measurement_interval){
            e=bus.word(0x4600,q.set_measurement_interval,10);
            if(!e)e=bus.command(0x0104,10);
            if(!e){running=false;e=start();}
            uint16_t actual=0;if(!e)e=bus.words(0x4600,&actual,1,10);
            if(!e&&actual!=q.set_measurement_interval)e=-EIO;
        }
        return e;
    }
};
}
