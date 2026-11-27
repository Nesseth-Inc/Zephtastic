// SPDX-License-Identifier: GPL-3.0-only
#include "StockFilesystem.h"
#include "HopScalingModule.h"
#include "stock/modules/HopScalingModule.h"
#include <cstdio>
#include <utility>
#include <cerrno>

StockConfig config{};
struct Donor : HopScalingModule { using HopScalingModule::runOnce; };
using Hist=mt::stock::HopScalingModule;
struct Store {
    std::vector<uint8_t> data;
    int readError=0,writeError=0;
    unsigned attempts=0;
    int read(const char *key,uint8_t *out,size_t capacity){
        assert(!std::strcmp(key,"mthop/state"));
        if(readError)return readError;
        if(data.empty())return -ENOENT;
        if(data.size()>capacity)return -E2BIG;
        std::memcpy(out,data.data(),data.size());return int(data.size());
    }
    int write(const char *key,const uint8_t *in,size_t length){
        assert(!std::strcmp(key,"mthop/state")&&length==524);++attempts;
        if(writeError)return writeError;
        data.assign(in,in+length);return 0;
    }
};

static unsigned checks=0,rolls=0,boots=0;
static void compare(const Hist &zt,const Donor &stock){
    for(unsigned request=0;request<=7;++request){
        assert(zt.limit(request,UINT32_MAX,meshtastic_PortNum_POSITION_APP)==
               std::min<unsigned>(request,stock.getLastRequiredHop()));
        assert(zt.limit(request,1,meshtastic_PortNum_POSITION_APP)==request);
        checks+=2;
    }
}
static void boot(Store &store,unsigned peers,unsigned ticks,bool warm){
    Hist zt;
    Donor stock; // invokes the actual donor loader
    zt.loadHistory(store);
    constexpr uint16_t chosen=0xc711;
    if(!warm)stock.setHashSeed(chosen);
    zt.seed(warm?0x7314:chosen); // normal RF/first-poll seed must preserve restored seed
    assert(stock.getHashSeed()==chosen);
    assert(stock.getLastRequiredHop()==7);
    ++boots;
    uint32_t now=concurrency::OSThread::startup;
    for(unsigned i=0;i<ticks;++i){
        for(unsigned j=0;j<peers;++j){
            uint32_t node=0x87651u+j*997u;
            unsigned hops=j<100?0:(j/71)%4;
            stock.samplePacketForHistogram(node,hops);zt.samplePacketForHistogram(node,hops);
        }
        const auto before=donorWrites;
        assert(!zt.due(now-1)&&zt.due(now));stock.s_testChannelUtil=31;
        const auto delay=stock.runOnce();zt.runOnce(now,31,config.device.role);
        const bool checkpoint=zt.takeCheckpoint();
        assert(checkpoint==(donorWrites!=before) && !zt.takeCheckpoint());
        if(checkpoint){
            ++rolls;assert(!zt.saveHistory(store));
            assert(donorFile.size()==522 && store.data.size()==524);
            assert(std::equal(donorFile.begin(),donorFile.end(),store.data.begin()));
            if(i==12 && warm && !peers)assert(stock.getLastRequiredHop()<7);
        }
        compare(zt,stock);now+=delay;
    }
}
static void rejected(Store bad){
    const auto original=bad.data;
    const auto before=bad.attempts;
    Hist zt;zt.loadHistory(bad);zt.seed(99);
    // RF continues even if the saved state cannot be trusted or read.
    for(uint32_t i=1;i<83;++i)zt.samplePacketForHistogram(i,0);
    for(unsigned tick=0;tick<=24;++tick){
        zt.runOnce(30000+tick*300000,31,0);
        assert(!zt.takeCheckpoint());
    }
    assert(zt.limit(7,UINT32_MAX,meshtastic_PortNum_POSITION_APP)<7);
    assert(bad.attempts==before && bad.data==original);
    ++checks;
}
static void reseal(Store &s){
    const auto crc=mt::ChannelRecord::checksum(s.data.data(),522);
    std::memcpy(s.data.data()+522,&crc,2);
}
int main(){
    Store store;
    boot(store,360,37,false);
    boot(store,0,13,true);
    boot(store,255,25,true);
    // Cold control with the identical post-restart RF input (none).
    donorFile.clear();Store empty;boot(empty,0,13,false);
    assert(empty.data!=store.data);
    Store bad=store;bad.readError=-EIO;rejected(bad);
    for(unsigned at:{0u,4u,5u,6u,7u,8u,24u,521u,522u,523u}){
        bad=store;bad.data[at]^=0x80;rejected(bad);
    }
    for(auto [at,v]:{std::pair<unsigned,unsigned>{0,0}, {4,9},{5,0},{5,3},{5,129},{6,3},{7,14}}){
        bad=store;bad.data[at]=uint8_t(v);reseal(bad);rejected(bad);
    }
    bad=store;bad.data[5]=8;bad.data[6]=4;reseal(bad);rejected(bad);
    for(unsigned size:{1u,521u,522u,523u,525u}){bad=store;bad.data.resize(size);rejected(bad);}
    Hist failed;failed.loadHistory(store);store.writeError=-ENOSPC;
    auto before=store.data;
    for(unsigned i=0;i<=12;++i)failed.runOnce(30000+i*300000,31,0);
    assert(failed.takeCheckpoint() && failed.saveHistory(store)==-ENOSPC && !failed.takeCheckpoint());
    assert(store.data==before);
    for(unsigned i=13;i<24;++i){failed.runOnce(30000+i*300000,31,0);assert(!failed.takeCheckpoint());}
    failed.runOnce(7230000,31,0);assert(failed.takeCheckpoint());
    std::printf("{\"status\":\"PASS\",\"comparisons\":%u,\"hourly_saves\":%u,\"compared_boots\":%u}\n",checks,rolls,boots);
}
