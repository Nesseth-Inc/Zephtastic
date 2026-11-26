// SPDX-License-Identifier: GPL-3.0-only
// SENXXSensor maintenance policy, Meshtastic 8e6a88d. Times in flash are UTC
// seconds; command waits, power continuity and retry limits use uptime.
#pragma once
#include "SensorBus.h"
namespace mt::stock::telemetry::accessory {
class SenHistory {
    struct Record {
        uint32_t version=1,model=0;
        uint8_t serial[32]{};
        uint32_t cleaning=0,captured=0;
        uint16_t voc[4]{};
        uint32_t checksum=0;
    };
    static_assert(sizeof(Record)==60 && offsetof(Record,checksum)==56);
    Bus &bus;const char *key;
    Record row{};
    bool dirty=false,restorePending=false;
    uint64_t flushAfter=0;
    static uint32_t sum(const Record &r){
        const auto *p=reinterpret_cast<const uint8_t*>(&r);uint32_t hash=2166136261u;
        for(size_t i=0;i<offsetof(Record,checksum);i++){hash^=p[i];hash*=16777619u;}
        return hash;
    }
    static bool populated(const uint16_t *w){return w[0]||w[1]||w[2]||w[3];}
public:
    SenHistory(Bus &b,const char *k):bus(b),key(k){}
    static bool credible(uint32_t utc){return utc>1514764800u;}
    int identify(uint32_t model){
        uint16_t raw[16];int e=bus.words(0xd033,raw,16,50);if(e)return e;
        Record id;id.model=model;
        for(unsigned i=0;i<32;i++){
            uint8_t c=i%2?uint8_t(raw[i/2]):uint8_t(raw[i/2]>>8);
            if(!c)break;
            if(c<0x20||c>0x7e)return -EILSEQ;
            id.serial[i]=c;
        }
        if(!id.serial[0])return -ENODEV;
        Record previous{};e=bus.load(key,&previous,sizeof(previous));
        if(e&&e!=-ENOENT&&e!=-EINVAL)return e;
        const bool match=!e&&previous.version==id.version&&previous.model==model&&
            !memcmp(previous.serial,id.serial,sizeof(id.serial))&&previous.checksum==sum(previous);
        row=match?previous:id;dirty=false;flushAfter=0;
        restorePending=match&&row.captured&&populated(row.voc);
        return 0;
    }
    int flush(){
        if(!dirty||bus.now()<flushAfter)return 0;
        row.checksum=sum(row);int e=bus.save(key,&row,sizeof(row));
        if(e)flushAfter=bus.now()+60000;
        else{dirty=false;flushAfter=0;}
        return e;
    }
    void cleaned(uint32_t utc){
        if(credible(utc)){row.cleaning=utc;dirty=true;(void)flush();}
    }
    bool due(uint32_t utc){
        if(!credible(utc))return false;
        if(!row.cleaning){cleaned(utc);return false;}
        return utc>row.cleaning&&utc-row.cleaning>604800;
    }
    // First acquisition of real time can occur long after the sensor was probed.
    bool canRestore(uint32_t utc){
        if(!restorePending||!credible(utc))return false;
        if(!credible(row.captured)||row.captured>utc||utc-row.captured>=600){
            restorePending=false;return false;
        }
        return true;
    }
    int restore(){
        int e=bus.command(0x6181,20,row.voc,4);
        if(!e)restorePending=false;
        return e;
    }
    int capture(uint32_t utc){
        uint16_t stage[4]{};int e=bus.words(0x6181,stage,4,20);
        if(e||!populated(stage))return e?e:-ENODATA;
        if(credible(utc)){
            memcpy(row.voc,stage,sizeof(stage));row.captured=utc;
            dirty=true;(void)flush();
        }
        return 0;
    }
    void markStopped(){restorePending=row.captured&&populated(row.voc);}
};
}
