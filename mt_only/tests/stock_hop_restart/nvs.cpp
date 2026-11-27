// SPDX-License-Identifier: GPL-3.0-only
// The shared harness supplies the real Zephyr settings and NVS implementation.
#define CONFIG_MT_HOP_HISTORY 1
#include "stock/modules/HopScalingModule.h"
#include "ZephyrSettings.h"
#include "bridge.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Hist=mt::stock::HopScalingModule;
using Bytes=std::vector<uint8_t>;
using Rows=std::map<std::string,Bytes>;
constexpr char Key[]="mthop/state";

static Bytes value(mt::ZephyrSettings &settings,const std::string &key){
    Bytes data(2048);
    const int n=settings.read(key.c_str(),data.data(),data.size());
    if(n==-ENOENT)return {};
    assert(n>0);data.resize(n);return data;
}
static void protect(mt::ZephyrSettings &settings,Rows &rows,const std::string &key,size_t length){
    Bytes bytes(length);
    for(unsigned i=0;i<length;++i)bytes[i]=uint8_t(i*29+rows.size()+1);
    assert(!settings.write(key.c_str(),bytes.data(),length));rows.emplace(key,bytes);
}
static void populate(mt::ZephyrSettings &settings,Rows &rows,unsigned sectors){
    // Current fresh T096 and conservative XIAO ledger occupancy, plus full
    // NodeJournals, an active waypoint record and five opaque Bluetooth bonds.
    struct Row { const char *key;unsigned n; };
    const Row base[]={{"mtbase/config",80},{"mtbase/state0",624},{"mtbase/state1",624},
        {"mtbase/nodes0",1572},{"mtbase/nodes1",1572},{"mtsat/epoch",16},
        {"mtmod/state0",128},{"mtmod/state1",128},{"mtble/state0",32},{"mtble/state1",32},
        {"mtway/history",960},{"mtidentity/state0",84},{"bt/id",7},{"bt/irk",16},{"bt/name",18},{"bt/hash",16}};
    for(auto r:base)protect(settings,rows,r.key,r.n);
    if(sectors==4){
        const Row fresh[]={{"mtpki/contacts0",640},{"mtpki/contacts1",640},{"mtpki/nonces0",64},
            {"mtpki/nonces1",64},{"mtpki/fresh0",56},{"mtpki/fresh-pending",84}};
        for(auto r:fresh)protect(settings,rows,r.key,r.n);
    }else{
        const Row ledger[]={{"mtidentity/state1",84},{"mtpki/ledger0",1944},{"mtpki/ledger1",1944},
            {"mttx/history0",82},{"mttx/history1",82}};
        for(auto r:ledger)protect(settings,rows,r.key,r.n);
    }
    for(unsigned peer=1;peer<=5;++peer){
        for(Row r: {Row{"keys",56},{"ccc",8},{"sc",4},{"cf",2}}){
            char key[40];snprintf(key,sizeof(key),"bt/%s/%012u0",r.key,peer);
            protect(settings,rows,key,r.n);
        }
    }
    model_remount();
}
static void verify(mt::ZephyrSettings &settings,const Rows &protectedRows){
    for(const auto &row:protectedRows)assert(value(settings,row.first)==row.second);
}
static void tickHour(Hist &hist,unsigned hour){
    for(unsigned i=1;i<=83;++i)hist.samplePacketForHistogram(11*i,(hour+i/41)%3);
    for(unsigned t=0;t<12;++t){
        hist.runOnce(330000+(hour*12+t)*300000,31,0);
        if(t<11)assert(!hist.takeCheckpoint());
    }
    assert(hist.takeCheckpoint()&&!hist.takeCheckpoint());
}
static unsigned interrupt(unsigned total,Hist &hist,mt::ZephyrSettings &settings,const Rows &rows,
                          const Bytes &old,const Bytes &updated){
    for(unsigned event=1;event<=total;++event){
        for(int phase=0;phase<3;++phase){
            model_restore();model_fail(event,phase);
            (void)hist.saveHistory(settings);
            model_no_fault();model_remount();
            const auto got=value(settings,Key);
            assert(got==old||got==updated);
            verify(settings,rows);
            Hist restarted;restarted.loadHistory(settings);restarted.seed(5678);
            assert(restarted.limit(7,UINT32_MAX,meshtastic_PortNum_NODEINFO_APP)==7);
            restarted.runOnce(30000,31,0);
            for(unsigned tick=0;tick<12;++tick)restarted.runOnce(330000+tick*300000,31,0);
            assert(restarted.takeCheckpoint());
            if(got.empty())assert(restarted.limit(7,UINT32_MAX,meshtastic_PortNum_NODEINFO_APP)==7);
            else assert(restarted.limit(7,UINT32_MAX,meshtastic_PortNum_NODEINFO_APP)<7);
            assert(!restarted.saveHistory(settings));model_remount();verify(settings,rows);
        }
    }
    return total*3;
}
static void exercise(unsigned sectors){
    model_reset(sectors);mt::ZephyrSettings settings;Rows rows;
    populate(settings,rows,sectors);verify(settings,rows);
    Hist hist;hist.loadHistory(settings);hist.seed(123);hist.runOnce(30000,31,0);
    unsigned cuts=0,collections=0,workloadErases=0;
    for(unsigned hour=0;hour<120;++hour){
        tickHour(hist,hour);
        const auto old=value(settings,Key);
        model_snapshot();model_fail(0,0);const auto eraseBefore=model_erases();
        assert(!hist.saveHistory(settings));
        const auto total=model_events(),erases=model_erases()-eraseBefore;
        workloadErases+=erases;
        const auto updated=value(settings,Key);
        assert(total>0 && updated.size()==524 && updated!=old);
        if(!hour || (erases&&!collections)){
            cuts+=interrupt(total,hist,settings,rows,old,updated);
            model_restore();assert(!hist.saveHistory(settings));
        }
        if(erases)++collections;
        if(hour%12==0)model_remount();
        assert(value(settings,Key)==updated);verify(settings,rows);
    }
    assert(collections&&cuts);
    // Retaining the checkpoint must still permit replacement of a large live
    // node journal, alongside every protected setting in this population.
    auto &journal=rows.at("mtbase/nodes0");journal[0]^=0xff;
    assert(!settings.write("mtbase/nodes0",journal.data(),journal.size()));
    model_remount();verify(settings,rows);
    assert(!settings.eraseVerified(Key));model_remount();
    assert(value(settings,Key).empty());verify(settings,rows);
    // Fill a fresh-key namespace down to 128-byte values, then verify that
    // first-checkpoint ENOSPC leaves all earlier owners byte-for-byte intact.
    unsigned fillers=0;
    for(unsigned size:{1024u,256u,128u}){
        Bytes filler(size,0x73);
        for(;;){
            assert(fillers<128);
            auto name="test/fill"+std::to_string(fillers++);
            const int rc=settings.write(name.c_str(),filler.data(),size);
            if(rc){assert(rc==-ENOSPC);break;}
            rows.emplace(name,filler);
        }
    }
    const int full=hist.saveHistory(settings);
    assert(full==-ENOSPC);
    model_remount();assert(value(settings,Key).empty());verify(settings,rows);
    std::printf("HOP_NVS %u %zu %u %u %u\n",sectors,rows.size(),cuts,collections,workloadErases);
}
int main(){exercise(4);exercise(8);}
