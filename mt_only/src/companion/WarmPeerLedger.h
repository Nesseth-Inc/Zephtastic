// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::companion {
// Meshtastic WarmNodeEntry at 8e6a88d, native little-endian on both targets.
// The remembered key is evidence for equality. The consumer never receives it.
struct WarmPeerRecord {
    uint32_t id=0,packed=0;
    uint8_t key[32]{};
    uint32_t lastHeard()const{return packed&~127u;}
    uint8_t role()const{return packed&15;}
    unsigned category()const{return (packed>>4)&3;}
    bool signedBefore()const{return packed&64;}
    bool keyed()const{for(auto b:key)if(b)return true;return false;}
    static uint32_t pack(uint32_t utc,uint8_t role,uint8_t category,bool signedBefore){
        return (utc&~127u)|(role&15u)|((category&3u)<<4)|(signedBefore?64u:0u);
    }
};
static_assert(sizeof(WarmPeerRecord)==40);

struct WarmPeerDomain {
    uint32_t self=0;
    uint8_t epoch[16]{},owner[32]{},channels[16]{},directory[16]{};
    bool valid()const{
        return self>1&&self<UINT32_MAX-1&&present(epoch)&&present(owner)&&present(directory);
    }
    template<unsigned N> static bool present(const uint8_t (&v)[N]){
        for(auto b:v)if(b)return true;
        return false;
    }
};
static_assert(sizeof(WarmPeerDomain)==84);

// The directory has already committed these withdrawals. Serial zero is initial,
// ring slot (serial-1)%4, and an ID of zero means all. Replaying removal twice is safe.
struct WarmPeerWithdrawals {
    uint32_t serial=0,ids[4]{};
    static constexpr unsigned Count=4;
};

template<unsigned Capacity> class WarmPeerLedger {
public:
#if defined(CONFIG_MT_PAGED_SATELLITES) && defined(CONFIG_MT_T096_FULL_COMPANION)
    static constexpr unsigned PerPage=Capacity==100?100:50;
#else
    static constexpr unsigned PerPage=50;
#endif
    static constexpr unsigned Pages=(Capacity+PerPage-1)/PerPage;
    static constexpr unsigned FormatCapacity=PerPage==50?Capacity:Capacity+0x10000;
    enum class Action:uint8_t {Demote,Take,Forget,Inspect,Promote};
private:
    static_assert(Capacity>0&&Capacity<=2000);
    struct Manifest {
        uint32_t magic=0x31574d5a,capacity=FormatCapacity;
        WarmPeerDomain domain{};
        uint8_t token[16]{};
        uint32_t serial=0,crc=0;
    } manifest_{};
    struct Page {
        uint8_t token[16]{};
        uint32_t number=0;
        WarmPeerRecord rows[PerPage]{};
        uint32_t crc=0;
    };
    static_assert(sizeof(Page)==24+PerPage*40);
    struct Choice {
        int slot=-1;
        uint32_t time=0;
        void consider(int candidate,uint32_t heard){
            if(slot<0||heard<time){slot=candidate;time=heard;}
        }
    };
    struct Operation {
        Action action=Action::Forget;
        WarmPeerRecord candidate{};
        unsigned page=0;
        int target=-1,empty=-1;
        Choice keyed{},keyless{};
    } job_{};
    bool open_=false,active_=false;
    static uint32_t crc(const void *ptr,size_t len){
        const auto *data=static_cast<const uint8_t*>(ptr);
        uint32_t result=UINT32_MAX;
        while(len--){result^=*data++;for(unsigned b=0;b<8;b++)result=(result>>1)^((0u-(result&1u))&0xedb88320u);}
        return ~result;
    }
    bool validId(uint32_t id)const{return id>1&&id<UINT32_MAX-1&&id!=manifest_.domain.self;}
    template<class IO> int commit(IO &io,Manifest &m){
        m.crc=crc(&m,offsetof(Manifest,crc));
        const int rc=io.put(-1,&m,sizeof(m));
        if(rc)close();else manifest_=m;
        return rc;
    }
    template<class IO> int startDomain(IO &io,const WarmPeerDomain &d,uint32_t serial){
        Manifest m{};m.domain=d;m.serial=serial;
        int rc=io.nonce(m.token,sizeof(m.token));
        if(rc||!WarmPeerDomain::present(m.token))return rc?rc:-EIO;
        if(!memcmp(manifest_.token,m.token,sizeof(m.token)))return -EIO;
        return commit(io,m);
    }
    template<class IO> int read(IO &io,unsigned number,Page &page){
        int rc=io.get(int(number),&page,sizeof(page));
        if(rc&&rc!=-ENOENT)return rc;
        if(!rc&&(page.number!=number||page.crc!=crc(&page,offsetof(Page,crc))))return -EILSEQ;
        if(rc==-ENOENT||memcmp(page.token,manifest_.token,sizeof(page.token))){
            page={};page.number=number;memcpy(page.token,manifest_.token,sizeof(page.token));return 0;
        }
        for(unsigned i=0;i<PerPage;i++){
            const auto &r=page.rows[i];
            if(r.id&&(!validId(r.id)||number*PerPage+i>=Capacity))return -EILSEQ;
        }
        return 0;
    }
    template<class IO> int finish(IO &io,Page &page,int slot,WarmPeerRecord &taken){
        auto &at=page.rows[unsigned(slot)%PerPage];
        WarmPeerRecord prior=at;
        if(job_.action==Action::Demote){
            at=job_.candidate;
            if(at.id==prior.id&&prior.signedBefore())at.packed|=64;
            if(!at.keyed()&&at.id==prior.id)memcpy(at.key,prior.key,sizeof(at.key));
        }else at={};
        active_=false;
        if(!memcmp(&at,&prior,sizeof(at)))return 0;
        page.crc=crc(&page,offsetof(Page,crc));
        const int rc=io.put(int(page.number),&page,sizeof(page));
        if(rc){close();return rc;}
        if(job_.action==Action::Take||job_.action==Action::Promote)taken=prior;
        return 0;
    }
public:
    void close(){active_=open_=false;}
    bool ready()const{return open_;}
    bool busy()const{return active_;}
    void cancel(){active_=false;}
    uint32_t serial()const{return manifest_.serial;}
    bool bound(const WarmPeerDomain &d)const{return open_&&!memcmp(&d,&manifest_.domain,sizeof(d));}
    template<class IO> int open(IO &io,const WarmPeerDomain &domain,uint32_t currentSerial){
        close();if(!domain.valid())return -EINVAL;
        int rc=io.get(-1,&manifest_,sizeof(manifest_));
        if(rc&&rc!=-ENOENT)return rc;
        if(!rc&&manifest_.magic==0x31574d5a&&manifest_.capacity!=FormatCapacity&&
           manifest_.crc==crc(&manifest_,offsetof(Manifest,crc)))return -EXDEV;
        if(!rc&&(manifest_.magic!=0x31574d5a||manifest_.capacity!=FormatCapacity||
            !WarmPeerDomain::present(manifest_.token)||manifest_.crc!=crc(&manifest_,offsetof(Manifest,crc))))return -EILSEQ;
        if(rc==-ENOENT||memcmp(&domain,&manifest_.domain,sizeof(domain))||currentSerial<manifest_.serial){
            rc=startDomain(io,domain,currentSerial);if(rc)return rc;
        }
        open_=true;return 0;
    }
    // Invalidate in constant time. Every live page has the nonce in the manifest.
    template<class IO> int clear(IO &io,uint32_t serial){
        if(!open_)return -EAGAIN;
        cancel();return startDomain(io,manifest_.domain,serial);
    }
    template<class IO> int acknowledge(IO &io,uint32_t serial){
        if(!open_||active_)return -EAGAIN;
        if(manifest_.serial==UINT32_MAX||serial!=manifest_.serial+1)return -ESTALE;
        auto m=manifest_;m.serial=serial;return commit(io,m);
    }
    int begin(Action action,const WarmPeerRecord &candidate){
        if(!open_||active_)return -EBUSY;
        if(!validId(candidate.id))return -EINVAL;
        job_={};job_.action=action;job_.candidate=candidate;active_=true;return 0;
    }
    // Exactly one page read per call. A completed mutation uses a synced temporary
    // plus atomic rename. Even when the selected victim is on an earlier page,
    // this call yields before re-reading it; there is no full ledger in RAM.
    template<class IO> int step(IO &io,WarmPeerRecord &taken){
        if(!active_)return -EINVAL;
        const unsigned number=job_.target>=0?unsigned(job_.target)/PerPage:job_.page;
        Page page{};int rc=read(io,number,page);
        if(rc){close();return rc;}
        if(job_.target>=0)return finish(io,page,job_.target,taken);
        for(unsigned i=0;i<PerPage&&number*PerPage+i<Capacity;i++){
            const int slot=int(number*PerPage+i);const auto &row=page.rows[i];
            if(row.id==job_.candidate.id){
                // A descriptive promotion is not durable evidence of signature verification.
                // Retain the negative identity guard across a restart between both owners.
                if(job_.action==Action::Inspect||(job_.action==Action::Promote&&row.signedBefore())){
                    taken=row;active_=false;return 0;
                }
                return finish(io,page,slot,taken);
            }
            if(job_.action!=Action::Demote)continue;
            if(!row.id){if(job_.empty<0)job_.empty=slot;}
            else (row.keyed()?job_.keyed:job_.keyless).consider(slot,row.lastHeard());
        }
        if(++job_.page<Pages)return -EINPROGRESS;
        if(job_.action!=Action::Demote){active_=false;return -ENOENT;}
        job_.target=job_.empty>=0?job_.empty:job_.keyless.slot>=0?job_.keyless.slot:
                    job_.candidate.keyed()?job_.keyed.slot:-1;
        if(job_.target<0){active_=false;return -ENOSPC;}
        return unsigned(job_.target)/PerPage==number?finish(io,page,job_.target,taken):-EINPROGRESS;
    }
};
}
