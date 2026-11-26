// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d AS3935Sensor and NAU7802Sensor, SparkFun v1.4.9/v1.0.6.
#pragma once
#include "SensorBus.h"
#include <float.h>
#if defined(CONFIG_MT_VEML7700)
#include "Veml7700.h"
#endif
#if defined(CONFIG_MT_ADS1X15)
#include "Ads1x15.h"
#endif
#if defined(CONFIG_MT_RAK12035)
#include "AirAndSoil.h"
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
#include "StockBarometers.h"
#endif
#if defined(CONFIG_MT_STOCK_SHT)
#include "StockSht.h"
#endif
namespace mt::stock::telemetry::accessory {
struct Environment {
#if defined(CONFIG_MT_OPT3001)
    bool hasOptLux=false;float optLux=0;
#endif
#if defined(CONFIG_MT_STOCK_SHT)
    ShtSample sht{};
#endif
#if defined(CONFIG_MT_BH1750)
    bool hasBhLux=false;float bhLux=0;
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
    bool hasTslLux=false,hasTsl2561Lux=false;float tslLux=0;
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
    BarometerSample barometer{};
#endif
#if defined(CONFIG_MT_VEML7700)
    VemlLight optical{};
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
    bool hasWeather=false,hasWeatherHumidity=false;
    float temperature=0,humidity=0,pressure=0,wind=0;
    uint16_t bearing=0;
#endif
#if defined(CONFIG_MT_RAK12035)
    SoilReadings soil{};
#endif
#if defined(CONFIG_MT_ADS1X15)
    AdcVoltages adc{};
#endif
    bool hasWeight=false,hasStrikes=false,hasDistance=false;
    float weight=0;
    bool hasRadiation=false,hasRange=false;
    float radiation=0,rangeMm=0;
    bool hasRain=false;float rainHour=0,rainDay=0;
    bool hasLight=false;float lux=0,uvIndex=0;
    uint32_t strikes=0;
    uint8_t distance=0;
    bool hasLastStrike=false;uint64_t lastStrikeMs=0;
};
class Lightning {
    Bus &bus;
    bool initialized=false;
    uint8_t trim=96,lastDistance=63;
    uint32_t buckets[12]{};
    uint64_t bucketMinute=0,lastStrike=0;bool everStruck=false;
    int writeTrim(uint8_t pf){
        int e=bus.update(8,0x0f,pf/8);uint8_t readback=0;
        if(!e)e=bus.readReg(8,&readback,1);
        return e?e:((readback&15)*8==pf?0:-EIO);
    }
    int init(){
        if(initialized)return 0;
        uint8_t saved=96;int e=bus.load("mtxsensor/as3935",&saved,1);
        if(e&&e!=-ENOENT)return e;
        if(saved>120||saved%8)return -EINVAL;
        bus.wait(4);e=bus.writeReg(0x3d,0x96);
        if(!e)e=bus.update(8,0xe0,0x40);
        if(!e){bus.wait(2);e=bus.update(8,0xe0,0);}
        uint8_t status[2]{};
        if(!e)e=bus.readReg(0x3a,status,2);
        if(!e&&((status[0]|status[1])&0x40))e=-EIO;
        if(!e)e=bus.update(0,0x3e,0x1c); // outdoor AFE gain
        if(!e)e=bus.update(1,0x7f,0x22); // noise floor 2, watchdog 2
        if(!e)e=bus.update(2,0x3f,2); // single strike threshold, spike rejection 2
        if(!e)e=bus.update(3,0x20,0x20);
        if(!e)e=writeTrim(saved);
        if(!e)e=bus.readReg(3,status,1); // drain stale interrupt
        if(!e){trim=saved;initialized=true;}
        return e;
    }
public:
    explicit Lightning(Bus &b):bus(b){}
    int tune(uint32_t pf){
        if(pf>120||pf%8)return -EINVAL;
        int e=init();if(e||trim==pf)return e;
        uint8_t desired=pf;e=writeTrim(desired);
        if(!e)e=bus.save("mtxsensor/as3935",&desired,1);
        if(e){if(writeTrim(trim))initialized=false;return e;}
        trim=desired;return 0;
    }
    int measure(Environment &out){
        int e=init();if(e)return e;
        const uint64_t epoch=bus.now()/300000;
        if(epoch<bucketMinute||epoch-bucketMinute>=12)memset(buckets,0,sizeof(buckets));
        else for(uint64_t i=bucketMinute+1;i<=epoch;i++)buckets[i%12]=0;
        bucketMinute=epoch;
        uint8_t irq;e=bus.readReg(3,&irq,1);if(e)return e;
        if((irq&15)==8){
            ++buckets[epoch%12];lastStrike=bus.now();everStruck=true;uint8_t d=63;
            if(bus.readReg(7,&d,1))d=63;
            lastDistance=d&63;
        }
        uint32_t total=0;for(uint32_t n:buckets)total+=n;
        out.hasStrikes=true;out.strikes=total;out.hasLastStrike=everStruck&&total;out.lastStrikeMs=lastStrike;
        out.hasDistance=total&&lastDistance!=63;out.distance=lastDistance;return 0;
    }
};

class Scale {
    struct Cal {int32_t zero=0;float factor=0;};
    static_assert(sizeof(Cal)==8);
    Bus &bus;Cal cal;bool initialized=false,calibrated=false;
    int waitBits(uint8_t reg,uint8_t mask,uint8_t expected,unsigned limit){
        for(unsigned i=0;i<=limit;i++){
            uint8_t v;int e=bus.readReg(reg,&v,1);if(e)return e;
            if((v&mask)==expected)return 0;
            if(i<limit)bus.wait(1);
        }
        return -ETIMEDOUT;
    }
    int power(){
        int e=bus.update(0,6,6);return e?e:waitBits(0,8,8,100);
    }
    int average(unsigned count,int32_t &out){
        const uint64_t start=bus.now();int64_t sum=0;
        for(unsigned have=0;have<count;){
            if(bus.now()-start>1000)return -ETIMEDOUT;
            uint8_t v;int e=bus.readReg(0,&v,1);if(e)return e;
            if(v&32){
                uint8_t raw[3];e=bus.readReg(0x12,raw,3);if(e)return e;
                const int32_t bits=int32_t(raw[0])<<16|uint16_t(raw[1])<<8|raw[2];
                sum+=bits&0x800000?bits-0x1000000:bits;++have;
            }
            if(have<count)bus.wait(1);
        }
        out=int32_t(sum/count);return 0;
    }
    int calibrateAfe(){
        int e=bus.update(2,7,4);
        if(!e)e=waitBits(2,4,0,1000);
        uint8_t status=0;if(!e)e=bus.readReg(2,&status,1);return e?e:((status&8)?-EIO:0);
    }
    int init(){
        if(initialized)return 0;
        Cal saved;int e=bus.load("mtxsensor/nau",&saved,sizeof(saved));
        if(e&&e!=-ENOENT)return e;
        const bool valid=e==0;
        if(valid&&(!(saved.factor>=-FLT_MAX&&saved.factor<=FLT_MAX)||saved.zero<-8388608||saved.zero>8388607))return -EINVAL;
        e=bus.update(0,1,1);
        if(!e){bus.wait(1);e=bus.update(0,1,0);}
        if(!e)e=power();
        if(!e)e=bus.update(1,0x3f,0x27); // 3.3 V LDO and gain 128
        if(!e)e=bus.update(0,0x80,0x80);
        if(!e)e=bus.update(2,0x70,0x30); // 80 SPS for initial AFE calibration
        if(!e)e=bus.update(0x15,0x30,0x30);
        if(!e)e=bus.update(0x1c,0x80,0x80);
        if(!e)e=bus.update(0x1b,0x40,0);
        if(!e){bus.wait(200);int32_t ignored;e=average(10,ignored);}
        if(!e)e=calibrateAfe();
        if(!e)e=bus.update(2,0x70,0x70); // stock 320 SPS
        if(!e)e=calibrateAfe();
        if(!e){cal=saved;calibrated=valid&&cal.factor!=0;initialized=true;}
        else (void)bus.update(0,6,0);
        return e;
    }
    int capture(unsigned n,int32_t &raw){
        int e=init();if(e)return e;
        e=power();
        if(!e)e=average(n,raw);
        const int off=bus.update(0,6,0);return e?e:off;
    }
public:
    explicit Scale(Bus &b):bus(b){}
    int set(uint32_t kg){
        int32_t raw=0;int e=capture(64,raw);if(e)return e;
        Cal candidate=cal;
        if(!kg)candidate.zero=raw;
        else {
            candidate.factor=(raw-cal.zero)/(float(kg)*1000);
            if(!candidate.factor)return -ERANGE;
        }
        e=bus.save("mtxsensor/nau",&candidate,sizeof(candidate));
        if(!e){cal=candidate;calibrated=cal.factor!=0;}
        return e;
    }
    int measure(Environment &out){
        int32_t raw=0;int e=capture(8,raw);if(e)return e;
        if(!calibrated)return -EAGAIN;
        const float kg=(raw-cal.zero)/(cal.factor*1000);
        out.weight=kg>0?kg:0;out.hasWeight=true;return 0;
    }
};
}
