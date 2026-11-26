// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WarmPeerLedger.h"
#include "../NodeCache.h"

namespace mt::companion {
inline WarmPeerRecord warmDemotion(const uint8_t *user,uint32_t utc){
    using E=NodeCache::Entry;
    WarmPeerRecord row{};row.id=E::get32(user);
    const uint8_t role=uint8_t(E::get32(user+8)),flags=user[E::LocalFlags];
    const bool signedBefore=flags&E::VerifiedSigner;
    const unsigned category=signedBefore?3:(flags&E::Protected)?2:(role==5||role==6||role==10)?1:0;
    row.packed=WarmPeerRecord::pack(utc,role,category,signedBefore);
    if(user[E::KeyLength]==32)memcpy(row.key,user+E::Key,sizeof(row.key));
    return row;
}

template<unsigned Capacity> class WarmPeerOwner {
    using Ledger=WarmPeerLedger<Capacity>;
    using Action=typename Ledger::Action;
    Ledger ledger_{};
    WarmPeerDomain domain_{};
    WarmPeerWithdrawals withdrawals_{};
    enum class Job:uint8_t{None,Demote,Take,Withdraw,Describe};
    Job job_;
    struct Observation {WarmPeerRecord row{};uint32_t epoch=0;};
    // Demotion is backpressured before the phone directory commits another
    // eviction. A promotion can be requeued from an admitted hot slot at capture.
    WarmPeerRecord demoted_{};
    Observation take_{},completed_{};
    struct Mark {uint32_t id,epoch;};
    Mark checked_[NodeCache::Capacity]{};
    bool absorbed_=false;
    struct Description{uint32_t id=0;uint8_t role=0;bool found=false,signer=false;};
    Description descriptions_[2]{};
    uint32_t wantedRole_=0;unsigned nextRole_=0;
    void invalidateRole(uint32_t id){
        if(!id||wantedRole_==id){
            wantedRole_=0;
            if(job_==Job::Describe){ledger_.cancel();job_=Job::None;}
        }
        for(auto &entry:descriptions_)if(!id||entry.id==id)entry={};
    }
    unsigned scan_=0;
    bool authorize_=false;
    void cancel(){job_=Job::None;ledger_.cancel();take_={};completed_={};}
public:
    // The directory worker creates the ledger's initial sentinels at startup.
    // Its empty record and observation buffers can then live in zero-init RAM.
    WarmPeerOwner() : job_(Job::None) {}
    static constexpr unsigned Pages=Ledger::Pages,ResetPages=(Capacity+49)/50;
    bool demotionSpace()const{return !demoted_.id;}
    bool absorbed()const{return absorbed_;}
    void acknowledged(){absorbed_=false;}
    void demote(const uint8_t *user,uint32_t utc){demoted_=warmDemotion(user,utc);invalidateRole(demoted_.id);}
    // An advisory role for the public Position duplicate interval only. A miss
    // queues one bounded lookup; neither signature nor key bytes cross this API.
    bool positionRole(uint32_t id,uint8_t &role){
        if(!authorize_||id<=1||id>=UINT32_MAX-1||id==domain_.self)return false;
        for(const auto &entry:descriptions_)if(entry.id==id){if(entry.found)role=entry.role;return entry.found;}
        if(!wantedRole_)wantedRole_=id;
        return false;
    }
    // A pending check can only veto this unsigned identity. Neither this lookup
    // nor any warm record can authorize a key, a signed packet or private traffic.
    bool refusesUnsignedIdentity(uint32_t id){
        if(!authorize_||!ledger_.bound(domain_)||ledger_.serial()!=withdrawals_.serial)return true;
        if(demoted_.id==id&&demoted_.signedBefore())return true;
        for(const auto &entry:descriptions_)if(entry.id==id)return entry.signer;
        if(!wantedRole_)wantedRole_=id;
        return true;
    }
    void suspend(){
        authorize_=false;cancel();ledger_.close();demoted_={};invalidateRole(0);
        memset(checked_,0,sizeof(checked_));absorbed_=false;scan_=0;
    }
    // Only copies RAM while the Service mutex is held. Directory durability
    // precedes this update. Retire queued old owners at the same boundary.
    void bind(const WarmPeerDomain &domain,const WarmPeerWithdrawals &fence){
        const bool changed=!authorize_||memcmp(&domain,&domain_,sizeof(domain));
        if(changed){suspend();domain_=domain;withdrawals_=fence;authorize_=domain.valid();return;}
        if(fence.serial!=withdrawals_.serial){
            const bool lost=fence.serial<withdrawals_.serial||fence.serial-withdrawals_.serial>fence.Count;
            if(lost){cancel();demoted_={};invalidateRole(0);}
            else for(uint32_t i=withdrawals_.serial;i<fence.serial;){
                const uint32_t id=fence.ids[i++%fence.Count];invalidateRole(id);
                if(!id||id==demoted_.id){demoted_={};if(job_==Job::Demote){job_=Job::None;ledger_.cancel();}}
                if(!id||id==take_.row.id||id==completed_.row.id)cancel();
            }
            withdrawals_=fence;
        }
    }
    // Compare a real admitted identity and its fresh durable enrollment before
    // applying any old descriptive time. No old flags, roles, or keys enter PKI.
    void capture(NodeCache &hot){
        if(!authorize_||!hot.ready())return;
        if(completed_.row.id){
            const int i=hot.find(completed_.row.id);
            if(i>=0&&hot.replayEpoch(unsigned(i))==completed_.epoch){
                const auto &e=*hot.get(unsigned(i));
                if(!e.ignored()&&!e.placeholder()&&!e.lastHeard()&&
                   e.bytes[NodeCache::Entry::KeyLength]==(completed_.row.keyed()?32:0)&&
                   !memcmp(e.bytes+NodeCache::Entry::Key,completed_.row.key,32))
                    (void)hot.observeLastHeard(e.id(),completed_.row.lastHeard());
            }
            completed_={};
        }
        if(take_.row.id||!hot.count())return;
        for(unsigned n=0;n<hot.count();n++){
            if(scan_>=hot.count())scan_=0;
            const unsigned slot=scan_++;
            const auto epoch=hot.replayEpoch(slot);const auto &e=*hot.get(slot);
            if(!epoch||!e.heard||e.ignored()||e.placeholder()||(e.bytes[NodeCache::Entry::KeyLength]!=0&&e.bytes[NodeCache::Entry::KeyLength]!=32)||
               (checked_[slot].id==e.id()&&checked_[slot].epoch==epoch))continue;
            // Multiple peers created by one NodeJournal flush share a generation.
            // Slot membership changes always clear the epoch until a later flush.
            checked_[slot]={e.id(),epoch};take_.row=warmDemotion(e.bytes,e.lastHeard());take_.epoch=epoch;break;
        }
    }
    // All storage work is performed by the owner after the Service mutex is released.
    template<class IO> int poll(IO &io){
        if(!authorize_)return -EAGAIN;
        if(!ledger_.bound(domain_)){
            job_=Job::None;return ledger_.open(io,domain_,withdrawals_.serial);
        }
        if(ledger_.serial()!=withdrawals_.serial){
            if(job_!=Job::Withdraw){ledger_.cancel();job_=Job::None;}
            const uint32_t next=ledger_.serial()+1;
            if(!next||withdrawals_.serial<next||withdrawals_.serial-ledger_.serial()>withdrawals_.Count){
                job_=Job::None;return ledger_.clear(io,withdrawals_.serial);
            }
            const auto id=withdrawals_.ids[(next-1)%withdrawals_.Count];
            if(!id)return ledger_.clear(io,withdrawals_.serial);
            if(job_==Job::None){
                WarmPeerRecord r{};r.id=id;
                const int rc=ledger_.begin(Action::Forget,r);if(rc)return rc;job_=Job::Withdraw;
            }
        }else if(job_==Job::None){
            int rc=0;
            if(demoted_.id){rc=ledger_.begin(Action::Demote,demoted_);if(!rc)job_=Job::Demote;}
            else if(wantedRole_){WarmPeerRecord r{};r.id=wantedRole_;rc=ledger_.begin(Action::Inspect,r);if(!rc)job_=Job::Describe;}
            else if(take_.row.id){rc=ledger_.begin(Action::Promote,take_.row);if(!rc)job_=Job::Take;}
            else return 0;
            if(rc)return rc;
        }
        WarmPeerRecord prior{};const int rc=ledger_.step(io,prior);
        if(rc==-EINPROGRESS)return 0;
        const auto done=job_;job_=Job::None;
        if(rc&&(!ledger_.ready()||(rc!=-ENOENT&&rc!=-ENOSPC)))return rc;
        if(done==Job::Withdraw)return ledger_.acknowledge(io,ledger_.serial()+1);
        if(done==Job::Demote){
            // A full-tier insertion may have displaced a cached advisory source.
            // Resolve both advisory slots again from the newly durable ledger.
            invalidateRole(0);demoted_={};absorbed_=true;
        }
        if(done==Job::Describe&&wantedRole_){
            descriptions_[nextRole_++%2]={wantedRole_,prior.role(),!rc,!rc&&prior.signedBefore()};wantedRole_=0;
        }
        if(done==Job::Take){
            invalidateRole(take_.row.id);
            if(!rc&&!memcmp(take_.row.key,prior.key,32)){completed_.row=prior;completed_.epoch=take_.epoch;}
            take_={};
        }
        return 0;
    }
};
}
