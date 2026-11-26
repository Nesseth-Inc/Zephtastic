// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/L76kSetup.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace stock {
std::vector<std::string> writes;
std::vector<int> delays;
struct Serial {void write(const char *s){writes.emplace_back(s);}} serial;
void delay(int ms){delays.push_back(ms);}
#include "setup-oracle.inc"
}

namespace {
using mt::gnss::l76k::Result;
struct Bus {
    uint32_t clock=0,elapsed=0,baud=0,device=0,cancelAt=UINT32_MAX;
    unsigned sendFail=0,speedFail=0,writes=0,acceptedProbe=0,swQueries=0;
    bool other=false;
    std::string software="$GPTXT,01,01,02,SW=URANUS4*00\r\n",input;
    size_t next=0;
    std::vector<uint32_t> rates,configTimes;
    std::vector<std::string> config,queries;
    bool active()const{return elapsed<cancelAt;}
    bool wait(uint32_t ms){clock+=ms;elapsed+=ms;return active();}
    uint32_t now()const{return clock;}
    bool speed(uint32_t s){rates.push_back(s);baud=s;return rates.size()!=speedFail;}
    bool read(char &b){if(next==input.size())return false;b=input[next++];return true;}
    bool send(const char *s){
        if(++writes==sendFail)return false;
        bool heard=baud==device;
        if(s==std::string(stock::hardware)){
            queries.emplace_back(s);
            if(heard&&other)input+="$GPTXT,01,01,02,HW=ATGM336H\r\n";
        }else if(s==std::string(stock::software)){
            queries.emplace_back(s);
            ++swQueries;
            if(heard&&swQueries>=acceptedProbe)input+=software;
        }else{
            config.emplace_back(s);configTimes.push_back(clock);
        }
        return true;
    }
};
}
int main(){
    stock::configure();
    const auto expectedRates=stock::rates();
    assert(stock::writes.size()==3 && stock::delays==std::vector<int>(3,250));
    for(size_t index=0;index<expectedRates.size();++index){
        Bus b;b.clock=UINT32_MAX-150u;b.device=expectedRates[index];b.acceptedProbe=index+1;
        assert(mt::gnss::l76k::setup(b)==Result::Configured);
        assert(b.rates==std::vector<uint32_t>(expectedRates.begin(),expectedRates.begin()+index+1));
        assert(b.baud==b.device && b.config==stock::writes);
        for(size_t i=0;i<3;++i){
            uint32_t next=i<2?b.configTimes[i+1]:b.clock;
            assert(next-b.configTimes[i]==uint32_t(stock::delays[i]));
        }
    }
    Bus absent;absent.input="$GPTXT,01,01,02,SW=stale\r\n";
    assert(mt::gnss::l76k::setup(absent)==Result::Passive);
    auto all=expectedRates;all.push_back(9600);
    assert(absent.rates==all && absent.config.empty() && absent.elapsed<11000);
    Bus atgm;atgm.device=115200;atgm.other=true;
    assert(mt::gnss::l76k::setup(atgm)==Result::Passive && atgm.config.empty() && atgm.baud==9600);
    for(const std::string &broken : std::vector<std::string>{
        "$GPTXT,01,01,02,SW=\r\n","$GPTXT,01,01,02,SW=URANUS (truncated)","$GPRMC,unrelated\r\n",
        "$GPTXT,01,01,02,SW="+std::string(800,'x')+"\r\n"}){
        Bus b;b.device=9600;b.software=broken;
        assert(mt::gnss::l76k::setup(b)==Result::Passive && b.config.empty());
    }
    Bus longVersion;longVersion.device=9600;longVersion.software="$GPTXT,01,01,02,SW="+std::string(200,'x')+"\r";
    assert(mt::gnss::l76k::setup(longVersion)==Result::Configured);
    for(unsigned time : {0u,100u,200u,605u,850u,980u}){
        Bus b;b.device=9600;b.cancelAt=time;
        assert(mt::gnss::l76k::setup(b)==Result::Cancelled && b.config.size()<3);
    }
    for(unsigned failed=1;failed<=5;++failed){
        Bus b;b.device=9600;b.sendFail=failed;
        assert(mt::gnss::l76k::setup(b)==Result::Error);
    }
    Bus failure;failure.speedFail=2;
    assert(mt::gnss::l76k::setup(failure)==Result::Error && failure.config.empty());
    puts("PASS pinned L76K probe order, receiver writes/delays, other CAS model, cancellation and switch to passive");
}
