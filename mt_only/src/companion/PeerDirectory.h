// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../NodeCache.h"
#if defined(CONFIG_MT_WARM_PEERS)
#include "WarmPeerLedger.h"
#if defined(CONFIG_MT_PAGED_SATELLITES)
#include "SatellitePages.h"
#endif
#endif
#include <stddef.h>
#include <errno.h>

namespace mt::companion {
// Private, best-effort phone history. The radio and the trust stores never read it.
// A page is committed in its inactive file before the manifest changes sides.
#if defined(CONFIG_MT_T096_FULL_COMPANION) && defined(CONFIG_MT_PAGED_SATELLITES)
inline constexpr unsigned DirectoryRowsPerPage=40;
#elif defined(CONFIG_MT_T096_FULL_COMPANION)
inline constexpr unsigned DirectoryRowsPerPage=32;
#else
inline constexpr unsigned DirectoryRowsPerPage=16;
#endif
template<unsigned Capacity,unsigned RowsPerPage=DirectoryRowsPerPage> class PeerDirectory {
    static_assert(Capacity>0&&Capacity<255);
public:
    // 32 x 98-byte rows fit one 4 KiB flash sector on the small full-companion volume.
    static_assert(RowsPerPage==16||RowsPerPage==32||RowsPerPage==40);
#if defined(CONFIG_MT_PAGED_SATELLITES)
    static constexpr unsigned ThinVersion=RowsPerPage==16?8:RowsPerPage==32?10:12;
#else
    static constexpr unsigned ThinVersion=RowsPerPage==16?2:4;
#endif
    static constexpr unsigned PerPage=RowsPerPage,Pages=(Capacity+PerPage-1)/PerPage;
    // Reset owns both banks of every filename used by either directory geometry.
    static constexpr unsigned ResetPages=(Capacity+15)/16;
    struct Domain {uint32_t node;uint8_t epoch[16],key[32],channels[16];};
    struct Row {
        uint8_t user[NodeCache::Entry::Size]{},utc[4]{};
        int8_t snr=0;uint8_t flags=0;
        uint32_t id()const{return NodeCache::Entry::get32(user);}
        bool valid(uint32_t self)const{
            return NodeCache::Entry::valid(user,self)&&!(user[NodeCache::Entry::LocalFlags]&
                (NodeCache::Entry::Ignored|NodeCache::Entry::Placeholder))&&!(flags&0xe0);
        }
        static Row copy(const NodeCache::Entry &e){
            Row r;memcpy(r.user,e.bytes,sizeof(r.user));memcpy(r.utc,e.lastHeardBytes,4);
            r.snr=e.snr;r.flags=uint8_t(e.hops|(e.heard?8:0)|(e.viaMqtt?16:0));return r;
        }
        void entry(NodeCache::Entry &e)const{
            e={};memcpy(e.bytes,user,sizeof(user));memcpy(e.lastHeardBytes,utc,4);
            e.snr=snr;e.hops=flags&7;e.heard=flags&8;e.viaMqtt=flags&16;
        }
    };
private:
    struct Manifest {
        uint32_t magic=0x3144505a,version=ThinVersion,nodeGeneration=0,nextGeneration=0,count=0;
        Domain domain{};
        uint32_t ids[Capacity]{},pages[Pages]{};
        uint8_t order[(Capacity+3)&~3u]{},withdrawn[((Capacity+31)/32)*4]{};
#if defined(CONFIG_MT_WARM_PEERS)
        uint8_t incarnation[16]{};
        WarmPeerWithdrawals withdrawals{};
        Row demotion{};uint8_t reserved[2]{};
#endif
        uint32_t crc=0;
    } index_{};
    struct Page {
        uint32_t generation=0;
        Row rows[PerPage]{};
#if defined(CONFIG_MT_PAGED_SATELLITES)
        uint32_t lifetimes[PerPage]{};
#endif
        uint32_t crc=0;
    };
    static_assert(sizeof(Page)<=4096&&sizeof(Manifest)<=4096);
    uint8_t blocked_[(Capacity+7)/8]{};
    // Hydrate timestamp-only RAM state one checked page per storage-worker poll.
    // No phone request, radio packet, or telemetry callback reads the filesystem.
    uint32_t heard_[Capacity]{};
#if defined(CONFIG_MT_WARM_PEERS)
    uint8_t priorSigners_[(Capacity+7)/8]{};
#endif
    void rememberPublicFlags(unsigned slot,const Row &row){
        heard_[slot]=NodeCache::Entry::get32(row.utc);
#if defined(CONFIG_MT_WARM_PEERS)
        const uint8_t bit=uint8_t(1u<<(slot%8));
        if(row.user[NodeCache::Entry::LocalFlags]&NodeCache::Entry::VerifiedSigner)priorSigners_[slot/8]|=bit;
        else priorSigners_[slot/8]&=uint8_t(~bit);
#endif
    }
    uint16_t unreadPages_=0;
    static_assert(Pages<=16);
    bool ready_=false;
    static uint32_t checksum(const void *p,size_t length){
        auto data=static_cast<const uint8_t*>(p);uint32_t crc=UINT32_MAX;
        for(size_t i=0;i<length;i++){crc^=data[i];for(int bit=0;bit<8;bit++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
        return ~crc;
    }
    static_assert(sizeof(Row)==98&&sizeof(Domain)==68);
    bool permittedGeneration(uint32_t gen)const{return gen==index_.nodeGeneration||gen==index_.nextGeneration;}
    bool withheld(unsigned slot)const{return index_.withdrawn[slot/8]&(1u<<(slot%8));}
    void release(unsigned slot){index_.withdrawn[slot/8]&=uint8_t(~(1u<<(slot%8)));blockSlot(slot,false);}
    int lookup(uint32_t id)const{
        for(unsigned i=0;i<index_.count;i++)if(index_.ids[index_.order[i]]==id)return int(i);
        return -1;
    }
    void blockSlot(unsigned slot,bool value){
        const auto mask=uint8_t(1u<<(slot%8));
        if(value)blocked_[slot/8]|=mask;else blocked_[slot/8]&=uint8_t(~mask);
    }
    bool validIndex()const{
        if(index_.magic!=0x3144505a||index_.count>Capacity||
           index_.crc!=checksum(&index_,offsetof(Manifest,crc)))return false;
#if defined(CONFIG_MT_WARM_PEERS)
        if(index_.version!=ThinVersion+1||!WarmPeerDomain::present(index_.incarnation)||
           (index_.demotion.id()&&!index_.demotion.valid(index_.domain.node)))return false;
#else
        if(index_.version!=ThinVersion)return false;
#endif
        for(unsigned i=0;i<index_.count;i++){
            const unsigned slot=index_.order[i];if(slot>=Capacity)return false;
            const auto id=index_.ids[slot];if(id<=1||id>=UINT32_MAX-1||id==index_.domain.node||!index_.pages[slot/PerPage])return false;
            for(unsigned j=0;j<i;j++)if(index_.ids[index_.order[j]]==id||index_.order[j]==slot)return false;
        }
        return true;
    }
    template<class IO> bool priorGeometry(IO &io){
#if !defined(CONFIG_MT_PAGED_SATELLITES)
        if constexpr(RowsPerPage==16)return false;
#endif
        // A valid manifest from the other partition profile has a longer page
        // index. Distinguish it from corruption and preserve it for recovery.
        constexpr unsigned extra=(ResetPages-Pages)*sizeof(uint32_t);
        uint8_t bytes[sizeof(Manifest)+extra]{};
        auto matches=[&](size_t size,unsigned version){
            if(io.get(-1,0,bytes,size))return false;
            return NodeCache::Entry::get32(bytes)==0x3144505a&&NodeCache::Entry::get32(bytes+4)==version&&
                   NodeCache::Entry::get32(bytes+size-4)==checksum(bytes,size-4);
        };
#if defined(CONFIG_MT_WARM_PEERS)
#if defined(CONFIG_MT_PAGED_SATELLITES)
        constexpr unsigned mid=(Capacity+31)/32;
        const size_t midSize=sizeof(Manifest)+(int(mid)-int(Pages))*int(sizeof(uint32_t));
        if(midSize<=sizeof(bytes)&&matches(midSize,5))return true;
        if(midSize<=sizeof(bytes)&&matches(midSize-(sizeof(Manifest)-offsetof(Manifest,incarnation)-4),4))return true;
#endif
        if(matches(sizeof(bytes),3))return true;
        return matches(offsetof(Manifest,incarnation)+sizeof(uint32_t)+extra,2);
#else
        return matches(sizeof(bytes),2);
#endif
    }
    template<class IO> int commit(IO &io){
        index_.crc=checksum(&index_,offsetof(Manifest,crc));
        const int rc=io.put(-1,0,&index_,sizeof(index_));if(rc)ready_=false;return rc;
    }
    template<class IO> int readPage(IO &io,unsigned page,Page &out){
        const auto generation=index_.pages[page];if(!generation){out={};return 0;}
        int rc=io.get(page,generation&1,&out,sizeof(out));
        if(!rc&&(out.generation!=generation||out.crc!=checksum(&out,offsetof(Page,crc))))rc=-EILSEQ;
        if(!rc&&(unreadPages_&(1u<<page))){
            for(unsigned row=0;row<PerPage&&page*PerPage+row<Capacity;row++){
                const unsigned slot=page*PerPage+row;
                if(!index_.ids[slot])continue;
                if(out.rows[row].id()!=index_.ids[slot]||!out.rows[row].valid(index_.domain.node)){rc=-EILSEQ;break;}
                rememberPublicFlags(slot,out.rows[row]);
            }
            if(!rc)unreadPages_&=uint16_t(~(1u<<page));
        }
        if(rc)ready_=false;return rc;
    }
#if defined(CONFIG_MT_WARM_PEERS)
    template<class IO> int newIncarnation(IO &io){
        int rc=io.nonce(index_.incarnation,sizeof(index_.incarnation));
        if(!rc&&!WarmPeerDomain::present(index_.incarnation))rc=-EIO;
        index_.version=ThinVersion+1;return rc;
    }
    template<class IO> int withdraw(IO &io,uint32_t id){
        auto &f=index_.withdrawals;
        if(f.serial==UINT32_MAX){int rc=newIncarnation(io);if(rc){ready_=false;return rc;}f={};}
        f.ids[f.serial++%f.Count]=id;
        return 0;
    }
#endif
public:
#if defined(CONFIG_MT_WARM_PEERS)
    bool refusesUnsignedIdentity(uint32_t id)const{
        if(!ready_)return true;
        const int ordinal=lookup(id);
        if(ordinal<0)return index_.demotion.id()==id&&
            (index_.demotion.user[NodeCache::Entry::LocalFlags]&NodeCache::Entry::VerifiedSigner);
        const unsigned slot=index_.order[ordinal];
        return !eligibleAt(ordinal)||(unreadPages_&(1u<<(slot/PerPage)))||
            (priorSigners_[slot/8]&(1u<<(slot%8)));
    }
    const Row &pendingDemotion()const{return index_.demotion;}
    template<class IO> int finishDemotion(IO &io){
        if(!ready_)return -EAGAIN;
        if(!index_.demotion.id())return 0;
        index_.demotion={};return commit(io);
    }
    const WarmPeerWithdrawals &withdrawals()const{return index_.withdrawals;}
    WarmPeerDomain warmDomain()const{
        WarmPeerDomain d{};
        if(ready_){d.self=index_.domain.node;memcpy(d.owner,index_.domain.key,32);
            memcpy(d.epoch,index_.domain.epoch,16);memcpy(d.channels,index_.domain.channels,16);
            memcpy(d.directory,index_.incarnation,16);}
        return d;
    }
#endif
    bool ready()const{return ready_;}
    unsigned count()const{return ready_?index_.count:0;}
    uint32_t generation()const{return index_.nodeGeneration;}
    bool domain(const Domain &d)const{return ready_&&!memcmp(&d,&index_.domain,sizeof(d));}
    bool eligibleAt(unsigned ordinal)const{
        const unsigned slot=index_.order[ordinal];return !withheld(slot)&&!(blocked_[slot/8]&(1u<<(slot%8)));
    }
    bool eligible(uint32_t id)const{
        if(!ready_)return false;const int found=lookup(id);return found>=0&&eligibleAt(found);
    }
    bool censusReady()const{return ready_&&!unreadPages_;}
    uint32_t lastHeardAt(unsigned ordinal)const{return heard_[index_.order[ordinal]];}
    template<class IO> int hydrateCensus(IO &io){
        if(!unreadPages_)return 0;
        unsigned page=0;while(!(unreadPages_&(1u<<page)))++page;
        Page data{};return readPage(io,page,data);
    }
    uint32_t at(unsigned ordinal)const{return ordinal<count()?index_.ids[index_.order[ordinal]]:0;}
    void block(uint32_t id){if(!ready_)return;const int found=lookup(id);if(found>=0)blockSlot(index_.order[found],true);}
    void close(){ready_=false;}
    template<class IO> int open(IO &io,const Domain &d,uint32_t journalGeneration){
        ready_=false;uint8_t present=0,key=0;for(auto b:d.epoch)present|=b;for(auto b:d.key)key|=b;
        if(!present||!key||d.node<=1||d.node>=UINT32_MAX-1)return -EINVAL;
        int rc=io.get(-1,0,&index_,sizeof(index_));
        if(rc==-EILSEQ&&priorGeometry(io))return -EXDEV;
#if defined(CONFIG_MT_WARM_PEERS)
        if(rc==-EILSEQ){
            // The thin manifest ends with its CRC at the incarnation offset.
            // Preserve names and pages when enabling warm peers on this layout.
            constexpr size_t bytes=offsetof(Manifest,incarnation);
            rc=io.get(-1,0,&index_,bytes+sizeof(uint32_t));
            if(rc)return rc;
            uint32_t oldCrc;memcpy(&oldCrc,index_.incarnation,sizeof(oldCrc));
            if(index_.version!=ThinVersion||oldCrc!=checksum(&index_,bytes))return -EILSEQ;
            index_.withdrawals={};index_.demotion={};memset(index_.reserved,0,sizeof(index_.reserved));
            if((rc=newIncarnation(io)))return rc;
            index_.crc=checksum(&index_,offsetof(Manifest,crc));
            if(!validIndex())return -EILSEQ;
            if((rc=commit(io)))return rc;
        }
#endif
        if(rc&&rc!=-ENOENT)return rc;
#if defined(CONFIG_MT_PAGED_SATELLITES)
        if(!rc&&index_.magic==0x3144505a&&index_.version!=ThinVersion+1&&index_.crc==checksum(&index_,offsetof(Manifest,crc)))return -EXDEV;
#endif
        if(!rc&&!validIndex())return -EILSEQ;
        if(rc==-ENOENT||memcmp(&d,&index_.domain,sizeof(d))||!permittedGeneration(journalGeneration)){
            index_={};index_.domain=d;index_.nextGeneration=index_.nodeGeneration=journalGeneration;
#if defined(CONFIG_MT_WARM_PEERS)
            if((rc=newIncarnation(io)))return rc;
#endif
            if((rc=commit(io)))return rc;
        }
        unreadPages_=0;
        for(unsigned i=0;i<index_.count;i++)unreadPages_|=1u<<(index_.order[i]/PerPage);
        memset(blocked_,0,sizeof(blocked_));ready_=true;return 0;
    }
    template<class IO> int fence(IO &io,uint32_t before,uint32_t after){
        if(!ready_||!permittedGeneration(before)||!after||after!=before+1)return -ESTALE;
        index_.nodeGeneration=before;index_.nextGeneration=after;
        for(unsigned i=0;i<sizeof(blocked_);i++)index_.withdrawn[i]|=blocked_[i];
        return commit(io);
    }
    template<class IO> int checkpoint(IO &io,uint32_t generation){
        if(!ready_)return -EAGAIN;if(generation==index_.nodeGeneration)return 0;
        for(auto b:blocked_)if(b)return -EAGAIN;
        if(!permittedGeneration(generation))return -ESTALE;
        index_.nodeGeneration=index_.nextGeneration=generation;return commit(io);
    }
    template<class IO> int forget(IO &io,uint32_t id){
        if(!ready_)return -EAGAIN;
#if defined(CONFIG_MT_WARM_PEERS)
        if(id<=1||id>=UINT32_MAX-1||id==index_.domain.node)return -EINVAL;
        int rc=withdraw(io,id);if(rc)return rc;
        if(index_.demotion.id()==id)index_.demotion={};
        const int pos=lookup(id);if(pos<0)return commit(io);
#else
        const int pos=lookup(id);if(pos<0)return 0;
#endif
        const unsigned slot=index_.order[pos];release(slot);index_.ids[slot]=0;
        memmove(index_.order+pos,index_.order+pos+1,index_.count-unsigned(pos)-1);index_.order[--index_.count]=0;
        return commit(io);
    }
    template<class IO> int clear(IO &io){
        if(!ready_)return -EAGAIN;
        const auto d=index_.domain;const auto generation=index_.nodeGeneration;
        unreadPages_=0;index_={};index_.domain=d;index_.nextGeneration=index_.nodeGeneration=generation;memset(blocked_,0,sizeof(blocked_));
#if defined(CONFIG_MT_WARM_PEERS)
        int rc=newIncarnation(io);if(rc){ready_=false;return rc;}
#endif
        return commit(io);
    }
    // Match getOrCreateMeshNode's oldest eligible "boring" (keyless) first,
    // followed by the oldest unprotected keyed peer. Iterating insertion
    // order breaks ties deterministically when the clock has not been set.
    template<class IO> int victim(IO &io,Page &page){
        uint32_t oldest=UINT32_MAX,keylessOldest=UINT32_MAX;int ordinary=-1,keyless=-1;
        unsigned loaded=Pages;
        for(unsigned i=0;i<index_.count;i++){
            const unsigned slot=index_.order[i],pageIndex=slot/PerPage;
            if(withheld(slot)||(blocked_[slot/8]&(1u<<(slot%8))))continue;
            if(pageIndex!=loaded){const int rc=readPage(io,pageIndex,page);if(rc)return rc;loaded=pageIndex;}
            const auto &row=page.rows[slot%PerPage];
            if(row.id()!=index_.ids[slot]||!row.valid(index_.domain.node)){ready_=false;return -EILSEQ;}
            const auto flags=row.user[NodeCache::Entry::LocalFlags];
            if(flags&(NodeCache::Entry::Favorite|NodeCache::Entry::Ignored|NodeCache::Entry::ManualVerified))continue;
            const auto heard=NodeCache::Entry::get32(row.utc);
            if(ordinary<0||heard<oldest){ordinary=int(i);oldest=heard;}
            if(!row.user[NodeCache::Entry::KeyLength]&&(keyless<0||heard<keylessOldest)){keyless=int(i);keylessOldest=heard;}
        }
        return keyless>=0?keyless:ordinary>=0?ordinary:-ENOSPC;
    }
#if defined(CONFIG_MT_PAGED_SATELLITES)
    using SaveRow=Row;
#else
    using SaveRow=const Row &;
#endif
    template<class IO> int save(IO &io,SaveRow row,Row *evicted=nullptr){
        if(evicted)*evicted={};
        if(!ready_||!row.valid(index_.domain.node))return -EINVAL;
        const int pos=lookup(row.id());int evict=-1;unsigned slot=0;Page data{};
        if(pos>=0)slot=index_.order[pos];else if(index_.count==Capacity){evict=victim(io,data);if(evict<0)return evict;slot=index_.order[evict];}
        else{while(slot<Capacity&&index_.ids[slot])++slot;if(slot==Capacity)return -EILSEQ;}
        const unsigned page=slot/PerPage;int rc=readPage(io,page,data);if(rc)return rc;
        auto &old=data.rows[slot%PerPage];
#if defined(CONFIG_MT_PAGED_SATELLITES)
        // A hot identity restored from NodeJournal has no receive time. A
        // background identity refresh cannot erase the current durable header.
        if(pos>=0&&!NodeCache::Entry::get32(row.utc)&&
           old.user[NodeCache::Entry::KeyLength]==row.user[NodeCache::Entry::KeyLength]&&
           !memcmp(old.user+NodeCache::Entry::Key,row.user+NodeCache::Entry::Key,32))
            memcpy(row.utc,old.utc,sizeof(row.utc));
#endif
        if(pos>=0&&!memcmp(&old,&row,sizeof(row))){
            const bool durable=withheld(slot);release(slot);return durable?commit(io):0;
        }
        if(index_.pages[page]==UINT32_MAX)return -EOVERFLOW;
        Row victimRow{};
        if(evicted&&evict>=0){
            if(!old.valid(index_.domain.node)||old.id()!=index_.ids[slot]){ready_=false;return -EILSEQ;}
            victimRow=old;
        }
        const auto previous=index_.pages[page];data.generation=previous+1;
#if defined(CONFIG_MT_PAGED_SATELLITES)
        // Generation cannot wrap and increases for every write to this physical
        // page. Including slot and directory incarnation makes a same-key new
        // enrollment distinct, even if its number is immediately reused.
        if(pos<0||old.user[NodeCache::Entry::KeyLength]!=row.user[NodeCache::Entry::KeyLength]||
           memcmp(old.user+NodeCache::Entry::Key,row.user+NodeCache::Entry::Key,32))
            data.lifetimes[slot%PerPage]=data.generation;
#endif
        old=row;
        data.crc=checksum(&data,offsetof(Page,crc));rc=io.put(page,data.generation&1,&data,sizeof(data));
        if(rc){ready_=false;return rc;}
        if(pos<0){
            if(evict>=0){memmove(index_.order+evict,index_.order+evict+1,index_.count-unsigned(evict)-1);--index_.count;}
            index_.order[index_.count++]=uint8_t(slot);index_.ids[slot]=row.id();
        }
#if defined(CONFIG_MT_WARM_PEERS)
        if(evicted&&evict>=0)index_.demotion=victimRow;
#endif
        index_.pages[page]=data.generation;release(slot);
        if((rc=commit(io)))return rc;
        rememberPublicFlags(slot,row);
        if(previous)io.retire(page,previous&1);if(evicted)*evicted=victimRow;return 0;
    }
#if defined(CONFIG_MT_PAGED_SATELLITES)
    template<class IO> int reclaimInactive(IO &io){
        if(!ready_)return -EAGAIN;
        int rc=io.dropStage();
        for(unsigned p=0;!rc&&p<Pages;p++){
            const unsigned live=index_.pages[p];
            for(unsigned bank=0;bank<2&&!rc;bank++)
                if(!live||bank!=(live&1))rc=io.dropBank(p,bank);
        }
        return rc;
    }
    template<class IO> int publicHeard(IO &io,const SatelliteOwner &owner,uint32_t utc){
        if(!eligible(owner.id))return -ESTALE;
        Row row{};int rc=read(io,owner.id,row);if(rc)return rc;
        // The worker serializes all file-backed access to this directory. The
        // caller just checked the full owner before updating this report.
        if(!utc||utc==NodeCache::Entry::get32(row.utc))return 0;
        NodeCache::Entry::put32(row.utc,utc);return save(io,row);
    }
    // Capture a RAM-only, short-lived page ticket at receive admission. Fail
    // closed if the owner's slot changed before the worker reads its key.
    uint64_t ticket(uint32_t id)const{
        if(!eligible(id))return 0;
        const unsigned slot=index_.order[lookup(id)];return uint64_t(index_.pages[slot/PerPage])<<8|(slot+1);
    }
    template<class IO> int binding(IO &io,uint32_t id,SatelliteOwner &owner,uint32_t &heard){
        const int pos=ready_?lookup(id):-1;if(pos<0)return -ENOENT;
        const unsigned slot=index_.order[pos];
        if(blocked_[slot/8]&(1u<<(slot%8)))return -EAGAIN;
        if(withheld(slot))return -ESTALE;
        Page page{};int rc=readPage(io,slot/PerPage,page);if(rc)return rc;
        const auto &row=page.rows[slot%PerPage];
        if(row.id()!=id||!row.valid(index_.domain.node)){ready_=false;return -EILSEQ;}
        owner.id=id;owner.slot=uint8_t(slot);owner.lifetime=page.lifetimes[slot%PerPage];
        owner.keySize=row.user[NodeCache::Entry::KeyLength];memcpy(owner.key,row.user+NodeCache::Entry::Key,32);
        heard=NodeCache::Entry::get32(row.utc);return owner.valid()?0:-ESTALE;
    }
#endif
    template<class IO> int read(IO &io,uint32_t id,Row &row){
        if(!eligible(id))return -ENOENT;
        const unsigned slot=index_.order[lookup(id)];Page page{};int rc=readPage(io,slot/PerPage,page);
        if(rc)return rc;
        const auto &r=page.rows[slot%PerPage];
        if(r.id()!=id||!r.valid(index_.domain.node)){ready_=false;return -EILSEQ;}
        row=r;return 0;
    }
};
}
