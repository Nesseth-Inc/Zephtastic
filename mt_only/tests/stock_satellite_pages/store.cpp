// SPDX-License-Identifier: GPL-3.0-only
#include "companion/SatellitePages.h"
#include "companion/PeerDirectory.h"
#include <map>
#include <vector>
#include <cassert>
#include <cstdio>
#include "../stock_warm_peers/Map.h"
using namespace mt::companion;
using E=mt::NodeCache::Entry;
template<unsigned N> static void saturated(){
    SatellitePages<N> archive;Map disk;
    uint8_t domain[16]={8};archive.bind(domain);
    std::map<uint32_t,SatelliteOwner> owners;
    auto sample=[&](unsigned n,unsigned k){
        SatelliteSample r{};r.kind=k;r.owner.id=n+100;r.owner.slot=uint8_t(n);r.owner.lifetime=1;
        r.owner.keySize=32;memset(r.owner.key,n%255+1,32);
        r.utc=1700000000+n;r.flags=29;r.channel=6;r.quarterSnr=-21;r.size=SatelliteSample::limit(k);
        memset(r.bytes,uint8_t(n+k),r.size);owners[r.owner.id]=r.owner;return r;
    };
    auto check=[&](const SatelliteOwner &o,uint32_t &age){
        auto it=owners.find(o.id);if(it==owners.end())return -ENOENT;
        if(!it->second.same(o))return -ESTALE;age=UINT32_MAX-o.id;return 0;
    };
    auto write=[&](const SatelliteSample &r){archive.start();int rc;
        do{rc=archive.step(disk,r,check);}while(rc==-EINPROGRESS);return rc;};
    for(unsigned i=0;i<N;i++)for(unsigned k=0;k<4;k++)assert(!write(sample(i,k)));
    assert(disk.files.size()==archive.Pages);
    SatelliteSample found;unsigned seen[4]{};
    for(unsigned k=0;k<4;k++)for(unsigned i=0;i<N;i++){
        assert(!archive.read(disk,k,i,found));assert(found.size==SatelliteSample::limit(k));
        assert(found.channel==6&&found.flags==29&&found.quarterSnr==-21);++seen[k];
    }
    auto add=sample(N,0);assert(!write(add));
    unsigned old=0,other=0;
    for(unsigned i=0;i<N;i++){
        assert(!archive.read(disk,0,i,found));old+=found.owner.id==99+N;
        assert(!archive.read(disk,1,i,found));other+=found.owner.id==99+N;
    }
    assert(!old&&other==1); // independent maps; eviction uses the current owner's last-heard.
    add.kind=SatelliteSample::Status;add.size=0;assert(!write(add));
    auto existing=sample(1,SatelliteSample::Status);existing.size=0;assert(!write(existing));
    unsigned present=0;for(unsigned i=0;i<N;i++)present+=!archive.read(disk,3,i,found);assert(present==N-1);
    const auto intact=disk.files[0];
    disk.files[0][4]^=1;assert(archive.read(disk,1,0,found)==-EILSEQ);disk.files[0]=intact;
    disk.files[0][100]^=1;assert(archive.read(disk,1,0,found)==-EILSEQ);disk.files[0]=intact;
    disk.files[0].back()^=1;assert(archive.read(disk,1,0,found)==-EILSEQ);disk.files[0]=intact;
    domain[0]++;archive.bind(domain);assert(archive.read(disk,1,0,found)==-ENOENT);
    printf("PASS %u independent public reports per kind in %u board pages, LRU, withdrawal, channel/owner-domain replacement\n",N,archive.Pages);
}
static void lifetimes(){
    using D=PeerDirectory<2,40>;D store;Map io;D::Domain d{};d.node=9;d.epoch[0]=d.key[0]=7;
    assert(!store.open(io,d,15));E e;e.identity(878,1,0);e.bytes[E::KeyLength]=32;e.bytes[E::Key]=1;
    auto r=D::Row::copy(e);assert(!store.save(io,r));uint32_t heard=0;SatelliteOwner owner{},newOwner{};
    auto t=store.ticket(878);assert(t&&!store.binding(io,878,owner,heard)&&owner.valid());
    e.identity(878,1,5);r=D::Row::copy(e);assert(!store.save(io,r)&&store.ticket(878)!=t);
    assert(!store.binding(io,878,newOwner,heard)&&owner.same(newOwner));
    assert(!store.forget(io,878)&&!store.save(io,r));assert(!store.binding(io,878,newOwner,heard)&&!owner.same(newOwner));
    owner=newOwner;e.bytes[E::Key]=9;r=D::Row::copy(e);assert(!store.save(io,r));
    assert(!store.binding(io,878,newOwner,heard)&&!owner.same(newOwner));
    D boot;assert(!boot.open(io,d,15)&&!boot.binding(io,878,owner,heard)&&owner.same(newOwner));
    puts("PASS exact-key and durable per-slot lifetime across restart, rename, same-key re-enrollment, rotation");
}
int main(){saturated<40>();saturated<199>();lifetimes();}
