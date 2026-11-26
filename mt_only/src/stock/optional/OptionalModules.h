// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d: ReplyBotModule, ReplyModule, GenericThreadModule and DropzoneModule.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "meshtastic/portnums.pb.h"
#include "UnitConversions.h"
#include "FixedDecimal.h"

namespace mt::stock::optional {
struct Request {
    uint32_t from, to, id;
    unsigned port, channel, hopStart, hopLimit;
    bool wantAck, wantResponse, hasBitfield, privatePacket;
    int16_t rssi;
    float snr;
    const uint8_t *payload;
    size_t length;
};
struct Reply {
    unsigned port, channel, hop;
    uint32_t to, requestId;
    bool wantAck, privatePacket;
    const char *text;
};
inline constexpr char ExampleAnswer[]="Message Received";

class ReplyBotModule {
    struct Cooldown {uint32_t from=0, last=0;};
    Cooldown entries_[8]{};
    unsigned next_=0;
    static bool space(uint8_t c){return c==' '||(c>='\t'&&c<='\r');}
    static bool command(const uint8_t *p,size_t n,const char *word){
        const size_t k=strlen(word);
        return n>=k&&!memcmp(p,word,k)&&(n==k||!p[k]||space(p[k]));
    }
public:
    static bool isCommand(const uint8_t *p,size_t n){
        if(!p)return false;
        while(n&&(*p==' '||*p=='\t')){++p;--n;}
        return command(p,n,"/ping")||command(p,n,"/hello")||command(p,n,"/test");
    }
    template<class Send> void handleReceived(const Request &q,uint32_t self,unsigned primary,unsigned hops,uint32_t now,Send send){
        const bool dm=q.to==self;
        if(q.port!=meshtastic_PortNum_TEXT_MESSAGE_APP||(!dm&&(q.channel!=primary||q.to!=UINT32_MAX))||
           !isCommand(q.payload,q.length))return;
        Cooldown *slot=nullptr;
        for(auto &e:entries_)if(e.from==q.from){slot=&e;break;}
        if(slot){
            if(uint32_t(now-slot->last)<(dm?15000u:60000u))return;
        } else {
            slot=&entries_[next_];
            next_=(next_+1)%8;
        }
        *slot={q.from,now};
        const int distance=q.hopStart<q.hopLimit||(!q.hopStart&&!q.hasBitfield)?-1:int(q.hopStart-q.hopLimit);
        const int rssi=q.rssi>0?q.rssi-200:q.rssi;
        char message[96],snr[48];
        fixedDecimal(q.snr,1,snr);
        snprintf(message,sizeof(message),"🎙️ Mic Check : %d Hops away | RSSI %d | SNR %s",distance,rssi,snr);
        send(Reply{meshtastic_PortNum_TEXT_MESSAGE_APP,q.channel,hops,q.from,0,false,q.privatePacket,message});
    }
};

struct ReplyModule {
    template<class Send> static void handleReceived(const Request &q,uint32_t self,unsigned responseHop,Send send){
        if(q.port==meshtastic_PortNum_REPLY_APP&&q.wantResponse&&(q.to==self||q.to==UINT32_MAX))
            send(Reply{q.port,q.channel,responseHop,q.from,q.id,q.wantAck,q.privatePacket,ExampleAnswer});
    }
};

struct Conditions {
    bool open=false,hasWeather=false;
    float windMps=0,pressureHpa=0,celsius=0;
    unsigned bearing=0;
};

class DropzoneModule {
    bool pending_=false;
    uint32_t received_=0;
    static bool match(const uint8_t *p,size_t n,const char *name){
        if(!p||!name)return false;
        const size_t len=strlen(name);
        constexpr char suffix[]=" conditions";
        return n>=len+sizeof(suffix)-1&&!strncasecmp(reinterpret_cast<const char*>(p),name,len)&&
               !strncasecmp(reinterpret_cast<const char*>(p+len),suffix,sizeof(suffix)-1);
    }
public:
    void handleReceived(const Request &q,const char *shortName,const char *longName,uint32_t now){
        if(q.port==meshtastic_PortNum_TEXT_MESSAGE_APP&&
           (match(q.payload,q.length,shortName)||match(q.payload,q.length,longName))){
            received_=now;pending_=true;
        }
    }
    bool due(uint32_t now){
        if(!pending_||uint32_t(now-received_)<5000)return false;
        pending_=false;return true;
    }
    static void format(const Conditions &obs,uint32_t utc,char (&out)[200]){
        const char *status=obs.open?"OPEN":"CLOSED";
        const unsigned h=utc%86400/3600,m=utc/60%60,s=utc%60;
        if(obs.hasWeather){
            const float knots=UnitConversions::MetersPerSecondToKnots(obs.windMps);
            const float inHg=UnitConversions::HectoPascalToInchesOfMercury(obs.pressureHpa);
            char speed[48],pressure[48],temperature[48];
            fixedDecimal(knots,2,speed);fixedDecimal(inHg,2,pressure);fixedDecimal(obs.celsius,2,temperature);
            snprintf(out,sizeof(out),"%s @ %02u:%02u:%02uz\nWind %s kts @ %u°\nBaro %s inHg %s°C",
                     status,h,m,s,speed,obs.bearing,pressure,temperature);
        } else {
            snprintf(out,sizeof(out),"%s @ %02u:%02u:%02u\nNo sensor found",status,h,m,s);
        }
    }
};

class GenericThreadModule {
    bool first_=true;
    uint32_t last_=0;
public:
    template<class Log> void poll(uint32_t now,Log log){
        if(!first_&&uint32_t(now-last_)<10000)return;
        if(first_){log("first time GenericThread running");first_=false;}
        log("GenericThread executing");last_=now;
    }
};
}
