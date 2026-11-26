// SPDX-License-Identifier: GPL-3.0-only
// Bounded Zephyr adapter to DFRobot_LarkWeatherStation 4de3a9c I2C getValue.
#pragma once
#include "OptionalModules.h"
namespace mt::stock::optional {
struct StationWeather : Conditions {float humidity=0;bool hasHumidity=false;};
class LarkWeather {
    unsigned key_=0;
    bool waiting_=false;
    uint32_t sent_=0,nextCheck_=0;
    static float decimal(const char *s) {
        while(*s==' '||(*s>='\t'&&*s<='\r'))++s;
        const bool negative=*s=='-';
        if(negative||*s=='+')++s;
        double value=0,scale=1;
        bool fraction=false;
        for(;*s;++s){
            if(*s>='0'&&*s<='9'){value=value*10+(*s-'0');if(fraction)scale*=10;}
            else if(*s=='.'&&!fraction)fraction=true;
            else break;
        }
        return float((negative?-value:value)/scale);
    }
    static unsigned bearing(const char *s) {
        static constexpr const char *points[]={"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};
        for(unsigned i=0;i<16;++i)if(!strcmp(points[i],s))return (i*45)/2;
        return 0;
    }
    // One bounded transaction per owner poll. Wait for the station's status
    // byte between commands without sleeping under a radio or settings lock.
    template<bool full,class Result,class Write,class Read> bool collect(uint32_t now,Result &obs,Write write,Read read) {
        static constexpr const char *keys[]={"Temp","Speed","Pressure","Dir","Humi"};
        constexpr unsigned count=full?5:4;
        if(key_>=count)return true;
        if(!waiting_){
            uint8_t command[11]{};
            command[1]=uint8_t(strlen(keys[key_]));memcpy(command+3,keys[key_],command[1]);
            if(!write(command,3+command[1]))return true;
            sent_=nextCheck_=now;waiting_=true;
        }
        if(int32_t(now-nextCheck_)<0)return false;
        uint8_t status=0;
        if(!read(&status,1)||status==0x63||uint32_t(now-sent_)>=2500)return true;
        if(status!=0x53){nextCheck_=now+50;return false;}
        uint8_t op=0xff,length[2]{};
        if(!read(&op,1)||op!=0||!read(length,2)||length[1]||!length[0]||length[0]>24)return true;
        char text[25]{};
        if(!read(reinterpret_cast<uint8_t*>(text),length[0]))return true;
        switch(key_){
        case 0:obs.celsius=decimal(text);break;
        case 1:obs.windMps=decimal(text);break;
        case 2:obs.pressureHpa=decimal(text);break;
        case 3:obs.bearing=bearing(text);obs.hasWeather=true;break;
        default:if constexpr(full){obs.humidity=decimal(text);obs.hasHumidity=true;}break;
        }
        waiting_=false;
        return ++key_==count;
    }
public:
    template<class Write,class Read> bool poll(uint32_t now,Conditions &obs,Write w,Read r){
        return collect<false>(now,obs,w,r);
    }
    template<class Write,class Read> bool pollEnvironment(uint32_t now,StationWeather &obs,Write w,Read r){
        return collect<true>(now,obs,w,r);
    }
};
}
