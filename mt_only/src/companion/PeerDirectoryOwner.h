// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PeerDirectory.h"
namespace mt::companion {
template<unsigned Capacity,unsigned Clients,unsigned Rows=DirectoryRowsPerPage> class PeerDirectoryOwner {
public:
    using Directory=PeerDirectory<Capacity,Rows>;
    using Domain=typename Directory::Domain;
    using Row=typename Directory::Row;
private:
    Directory directory_;
    struct Read {uint32_t id=0;bool loaded=false;Row row{};};
    Read reads_[Clients]{};
    Row dirty_[4]{};unsigned used_=0;
    bool rescan_;
#if defined(CONFIG_MT_PAGED_SATELLITES)
    int openError_=-EAGAIN;
#else
    bool ownsManifest_=false;
#endif
    uint32_t lastScan_=0,revision_;
    void changeRevision(){if(!++revision_)++revision_;}
    void cancelReads(uint32_t id){
        for(auto &r:reads_)if(!id||r.id==id)r={};
    }
public:
    // Construct the empty page cache at startup. A constant-initialized owner
    // puts its otherwise empty read buffers into the board's flash image.
    PeerDirectoryOwner() : rescan_(true), revision_(1) {}
#if defined(CONFIG_MT_WARM_PEERS)
    bool refusesUnsignedIdentity(uint32_t id)const{
        for(unsigned i=0;i<used_;++i)
            if(dirty_[i].id()==id&&(dirty_[i].user[NodeCache::Entry::LocalFlags]&NodeCache::Entry::VerifiedSigner))
                return true;
        return directory_.refusesUnsignedIdentity(id);
    }
    const auto &pendingDemotion()const{return directory_.pendingDemotion();}
    template<class IO> int finishDemotion(IO &io){return directory_.finishDemotion(io);}
    const auto &warmWithdrawals()const{return directory_.withdrawals();}
    auto warmDomain()const{return directory_.warmDomain();}
#endif
#if defined(CONFIG_MT_PAGED_SATELLITES)
    template<class IO> int reclaimInactive(IO &io){return directory_.reclaimInactive(io);}
    template<class IO> int publicHeard(IO &io,const SatelliteOwner &owner,uint32_t utc){
        const int result=directory_.publicHeard(io,owner,utc);
        if(!result&&utc)for(auto &read:reads_)
            if(read.loaded&&read.id==owner.id)NodeCache::Entry::put32(read.row.utc,utc);
        return result;
    }
    uint64_t ticket(uint32_t id)const{return directory_.ticket(id);}
    template<class IO> int binding(IO &io,uint32_t id,SatelliteOwner &owner,uint32_t &heard){
        return directory_.binding(io,id,owner,heard);
    }
#endif
    bool ready()const{return directory_.ready();}
    uint32_t revision()const{return revision_;}
    bool domain(const Domain &d)const{return directory_.domain(d);}
    unsigned visibleCount(const NodeCache &hot,uint32_t utc=0,uint16_t *online=nullptr)const{
        unsigned result=hot.count();
        const bool measured=online&&utc&&directory_.censusReady();
        uint16_t recent=measured?hotOnline(hot,utc):0;
        for(unsigned i=0;i<directory_.count()&&result<Capacity;i++){
            const auto id=directory_.at(i);
            if(hot.find(id)>=0||!directory_.eligibleAt(i))continue;
            ++result;
            if(measured&&onlineAt(directory_.lastHeardAt(i),utc))++recent;
        }
        if(online)*online=recent;
        return result;
    }
    void disconnect(unsigned owner){if(owner<Clients)reads_[owner]={};}
    template<class IO> int open(IO &io,const Domain &d,uint32_t gen){
        changeRevision();used_=0;lastScan_=0;rescan_=true;cancelReads(0);
        const int rc=directory_.open(io,d,gen);
#if defined(CONFIG_MT_PAGED_SATELLITES)
        openError_=rc;
#else
        ownsManifest_=!rc;
#endif
        return rc;
    }
    template<class IO> int fenceJournal(IO &io,uint32_t before,uint32_t after){
#if defined(CONFIG_MT_PAGED_SATELLITES)
        // A foreign store can still have an outstanding one-generation fence.
        // Preserve its bytes and block the edit until this layout has a verified
        // durable fence. Media errors and space pressure must remain retryable.
        if(!ready())return openError_?openError_:-EAGAIN;
        return directory_.fence(io,before,after);
#else
        if(!ownsManifest_)return 0;
        if(ready()&&!directory_.fence(io,before,after))return 0;
        close();
        const int rc=io.discardManifest();if(!rc)ownsManifest_=false;
        return rc;
#endif
    }
    void close(){changeRevision();directory_.close();used_=0;rescan_=true;cancelReads(0);}
    void invalidate(uint32_t id){
        changeRevision();directory_.block(id);cancelReads(id);
        for(unsigned i=0;i<used_;i++)if(dirty_[i].id()==id){dirty_[i]=dirty_[--used_];break;}
    }
    template<class IO> int forget(IO &io,uint32_t id){
        invalidate(id);return directory_.forget(io,id);
    }
    template<class IO> int clear(IO &io){
        changeRevision();used_=0;cancelReads(0);rescan_=true;return directory_.clear(io);
    }
    void observe(const NodeCache::Entry &e){observe(e,e.lastHeard());}
    void observe(const NodeCache::Entry &e,uint32_t utc){
        if(!ready()||e.ignored()||e.placeholder())return;
        auto row=Row::copy(e);NodeCache::Entry::put32(row.utc,utc);
        directory_.block(e.id());cancelReads(e.id());
        for(unsigned i=0;i<used_;i++)if(dirty_[i].id()==e.id()){dirty_[i]=row;return;}
        if(used_==4){rescan_=true;return;}
        dirty_[used_++]=row;
    }
    // Polling may yield on FS work or a full transport queue. Advance the
    // index only after a peer is emitted or definitively skipped.
    int next(unsigned owner,unsigned ordinal,const NodeCache &hot,NodeCache::Entry &entry){
        if(owner>=Clients||ordinal>=directory_.count())return 0;
        const auto id=directory_.at(ordinal);
        if(!directory_.eligible(id)||hot.find(id)>=0)return 2;
        return identity(owner,id,entry);
    }
    int identity(unsigned owner,uint32_t id,NodeCache::Entry &entry){
        if(owner>=Clients||!directory_.eligible(id))return -ENOENT;
        auto &r=reads_[owner];
        if(r.id==id&&r.loaded){r.row.entry(entry);return 1;}
        r.id=id;r.loaded=false;return -EAGAIN;
    }
    // A phone edit may select a cold row by number. Local PKI remains the
    // only owner of an enrolled key; storage metadata cannot confer trust.
    template<class Pin> int favoriteCandidate(unsigned owner,uint32_t id,const NodeCache &hot,
                                              NodeCache::Entry &result,Pin pin){
        if(hot.find(id)>=0)return -ESTALE;
        const int status=identity(owner,id,result);if(status!=1)return status;
        if(hot.protectedCount()>=NodeCache::ProtectedLimit)return -ENOSPC;
        using E=NodeCache::Entry;
        uint8_t key[32]{};
        result.bytes[E::LocalFlags]=0;
        if(pin(id,key)){
            if(result.bytes[E::KeyLength]!=32||memcmp(key,result.bytes+E::Key,sizeof(key)))return -ESTALE;
        }else{
            result.bytes[E::KeyLength]=0;memset(result.bytes+E::Key,0,sizeof(key));
        }
        return 0;
    }
    // Copy hot state under the service mutex. Each observation is a complete
    // bounded record. The store phase runs outside that mutex.
    void capture(const NodeCache &hot,uint32_t gen,unsigned &cursor){
        capture(hot,gen,cursor,[this](const NodeCache::Entry &e){observe(e);});
    }
    template<class Observe> void capture(const NodeCache &hot,uint32_t gen,unsigned &cursor,Observe enqueue){
        if(!ready()||!hot.ready())return;
        if(!rescan_&&gen==lastScan_)return;
        if(cursor>=hot.count()){rescan_=false;lastScan_=gen;cursor=0;return;}
        if(used_<4){const auto *e=hot.get(cursor++);if(e)enqueue(*e);}
    }
    template<class IO> int poll(IO &io,uint32_t gen,Row *evicted=nullptr,bool maySave=true){
        if(evicted)*evicted={};
        if(!ready())return -EAGAIN;
        for(auto &r:reads_)if(r.id&&!r.loaded){
            if(!directory_.eligible(r.id)){r={};continue;}
            const int rc=directory_.read(io,r.id,r.row);if(rc){r={};return rc;}
            r.loaded=true;return 0;
        }
        if(!directory_.censusReady())return directory_.hydrateCensus(io);
        if(used_){
            if(!maySave)return 0;
            int rc=directory_.save(io,dirty_[0],evicted);if(rc&&rc!=-ENOSPC)return rc;
            --used_;memmove(dirty_,dirty_+1,used_*sizeof(Row));return 0;
        }
        return rescan_||lastScan_!=gen?0:directory_.checkpoint(io,gen);
    }
};
}
