// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "NodeAdmin.h"
#include "relay_fixture_provider.h"
#include <string>
#include "companion/PeerDirectoryOwner.h"
#include <map>
#include <algorithm>
#include <set>
#include <vector>

struct Store {
    std::map<std::pair<int,unsigned>,std::vector<uint8_t>> files;
    int fail=-2;
    unsigned reads=0,writes=0;
    int get(int p,unsigned b,void *dst,size_t n){
        ++reads;auto i=files.find({p,b});if(i==files.end())return -ENOENT;
        if(i->second.size()!=n)return -EILSEQ;memcpy(dst,i->second.data(),n);return 0;
    }
    int put(int p,unsigned b,const void *src,size_t n){
        if(fail==p)return -EIO;
        ++writes;auto data=static_cast<const uint8_t*>(src);files[{p,b}]={data,data+n};return 0;
    }
    void retire(int p,unsigned b){files.erase({p,b});}
};
static mt::NodeCache::Entry entry(uint32_t id,unsigned key=1){
    mt::NodeCache::Entry n;n.identity(id,81,0);
    snprintf(reinterpret_cast<char*>(n.bytes+n.Name),40,"peer-%u",unsigned(id));
    strcpy(reinterpret_cast<char*>(n.bytes+n.Short),"ABC");n.bytes[n.KeyLength]=32;
    memset(n.bytes+n.Key,int(key),32);n.heard=true;n.hops=2;n.snr=5;
    mt::NodeCache::Entry::put32(n.lastHeardBytes,1600000000u+id);return n;
}
struct NodeStore:mt::SettingsBackend{
    std::map<std::string,std::vector<uint8_t>> records;
    int read(const char *key,uint8_t *out,size_t n)override{
        auto i=records.find(key);if(i==records.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;
        memcpy(out,i->second.data(),i->second.size());return int(i->second.size());
    }
    int write(const char *key,const uint8_t *src,size_t n)override{records[key]={src,src+n};return 0;}
};
template<unsigned Limit> using Owner=mt::companion::PeerDirectoryOwner<Limit,4>;
template<unsigned Limit> struct Phone:Host{
    Owner<Limit> &directory;
    static inline Owner<Limit> *promoting=nullptr;
    static int invalidateCandidate(uint32_t id){promoting->invalidate(id);return 0;}
    bool blocked=false;
    struct Pause {
        bool busy=false;
        bool pause(){return !busy;}
        bool resume(){return true;}
        void hold(){assert(false);}
    } admission;
    mt::NodeJournal journal;NodeStore journalStore;uint32_t pinnedNode=0;uint8_t pin=0;
    uint32_t directoryRevision()const override{return directory.revision();}
    int promoteDirectoryFavorite(unsigned owner,uint32_t id,uint32_t now)override{
        mt::NodeCache::Entry candidate{};
        const int rc=directory.favoriteCandidate(owner,id,s.nodes,candidate,[&](uint32_t peer,uint8_t *key){
            if(peer!=pinnedNode)return false;memset(key,pin,32);return true;
        });
        if(rc)return rc;
        return mt::NodeAdmin::promoteFavorite(admission,[&](){
            promoting=&directory;
            const int result=journal.ownerContact(journalStore,s.nodes,candidate,false,false,now,true,true,0,invalidateCandidate);
            promoting=nullptr;return result;
        });
    }
    Phone(mt::Service &s,Owner<Limit>&d):Host(s),directory(d){}
    unsigned directoryLimit()const override{return Limit;}
    unsigned directoryCount(const mt::NodeCache &hot)const override{return directory.visibleCount(hot);}
    int directoryPeer(unsigned owner,unsigned index,const mt::NodeCache &hot,mt::NodeCache::Entry &e)override{return directory.next(owner,index,hot,e);}
    void directoryDisconnect(unsigned o)override{directory.disconnect(o);}
    bool send(const uint8_t *p,size_t n)override{return !blocked&&Host::send(p,n);}
};
template<unsigned Limit> static void directoryCase(){
    auto f=std::make_unique<Fixture>();Owner<Limit> archive;Store io;
    typename Owner<Limit>::Domain d{};d.node=OURS;d.epoch[0]=9;d.key[0]=1;d.channels[0]=3;
    constexpr unsigned gen=21;
    assert(!archive.open(io,d,gen));
    for(unsigned i=1;i<=Limit+2;i++){
        const auto peer=entry(1000+i);
        assert(f->s.nodes.accept(peer,OURS,5,2)>=0);archive.observe(peer);assert(!archive.poll(io,gen));
    }
    assert(archive.visibleCount(f->s.nodes)==Limit);
    // At most one published page per physical group and the manifest.
    assert(io.files.size()<=(Limit+15)/16+1);
    Owner<Limit> restarted;assert(!restarted.open(io,d,gen));Phone<Limit> h(f->s,restarted);
    mt::companion::CompanionWorkspace ws;mt::Companion usb(f->s,h,81,ws,0);
    auto handshake=[&](unsigned nonce){
        std::vector<uint32_t> ids;h.out.clear();
        meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=nonce;
        input(usb,request,1);
        for(unsigned i=0;i<Limit*4+100;i++){
            const unsigned before=io.reads;
            h.blocked=i%7==0;usb.poll(mt::Companion::WorkspaceProof{true});assert(before==io.reads);h.blocked=false;
            if(!h.out.empty()&&h.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)break;
            assert(!restarted.poll(io,gen));
        }
        assert(!h.out.empty()&&h.out.back().config_complete_id==nonce);
        for(const auto &v:h.out){
            if(v.which_payload_variant==meshtastic_FromRadio_my_info_tag)assert(v.my_info.nodedb_count==1+restarted.visibleCount(f->s.nodes));
            if(v.which_payload_variant!=meshtastic_FromRadio_node_info_tag)continue;
            ids.push_back(v.node_info.num);
            if(v.node_info.num!=OURS){assert(v.node_info.has_user&&!v.node_info.has_position&&!v.node_info.has_device_metrics);}
        }
        std::set<uint32_t> unique(ids.begin(),ids.end());assert(unique.size()==ids.size());
        return ids;
    };
    const auto first=handshake(69421);assert(first.size()==Limit+1&&first.front()==OURS);
    assert(std::find(first.begin(),first.end(),1001)==first.end());
    const auto second=handshake(77);assert(first==second);
    const unsigned id=1003;
    mt::NodeCache::Entry cold{};assert(restarted.next(0,0,f->s.nodes,cold)==-EAGAIN);
    assert(!restarted.poll(io,gen));assert(!restarted.forget(io,id));assert(restarted.visibleCount(f->s.nodes)==Limit-1);
    auto after=handshake(69421);assert(std::find(after.begin(),after.end(),id)==after.end());
    auto changed=entry(id,8);restarted.observe(changed);assert(!restarted.poll(io,gen));
    after=handshake(69421);assert(std::find(after.begin(),after.end(),id)!=after.end());
    for(const auto &v:h.out)if(v.which_payload_variant==meshtastic_FromRadio_node_info_tag&&v.node_info.num==id)
        assert(v.node_info.user.public_key.size==32&&v.node_info.user.public_key.bytes[0]==8);
    // A cut between the staged page and the new index retains the old version.
    const unsigned previousWrites=io.writes;io.fail=-1;restarted.observe(entry(1004,9));assert(restarted.poll(io,gen)==-EIO);
    assert(!restarted.ready()&&io.writes==previousWrites+1);io.fail=-2;
    assert(!restarted.open(io,d,gen));assert(restarted.visibleCount(f->s.nodes)==Limit);
    assert(!restarted.clear(io));assert(restarted.visibleCount(f->s.nodes)==f->s.nodes.count());
    restarted.observe(entry(115));assert(!restarted.poll(io,gen));
    assert(!restarted.open(io,d,gen+1)&&restarted.visibleCount(f->s.nodes)==f->s.nodes.count());
    restarted.observe(entry(117));assert(!restarted.poll(io,gen+1));d.epoch[0]++;
    assert(!restarted.open(io,d,gen+1)&&restarted.visibleCount(f->s.nodes)==f->s.nodes.count());
    io.files[{-1,0}][0]^=1;assert(restarted.open(io,d,gen+1)==-EILSEQ&&!restarted.ready());
    printf("PASS %u retained thin peers plus self, real config and nodes-only handshake, reboot, backpressure, hot deduplication, remove/re-key, reset, crash/epoch rejection\n",Limit);
}
static void durability(){
    using D=Owner<4>::Directory;using E=mt::NodeCache::Entry;
    D::Domain d{};d.node=OURS;d.epoch[0]=1;d.key[0]=3;
    D base;Store io;assert(!base.open(io,d,21));
    auto a=entry(201);a.bytes[E::LocalFlags]=E::Favorite;
    auto b=entry(202);auto c=entry(203);c.bytes[E::KeyLength]=0;memset(c.bytes+E::Key,0,32);
    auto recent=entry(204);E::put32(recent.lastHeardBytes,1999999999);
    for(auto peer:{a,b,c,recent})assert(!base.save(io,D::Row::copy(peer)));
    assert(!base.save(io,D::Row::copy(entry(205)))&&base.eligible(201)&&base.eligible(202)&&!base.eligible(203));
    assert(!base.save(io,D::Row::copy(entry(206)))&&base.eligible(201)&&!base.eligible(202)&&base.eligible(204));
    // An admitted update is withdrawn before either journal side can become active.
    // A reset during the manifest write retains the previous complete manifest.
    base.block(206);Store cutBefore=io;cutBefore.fail=-1;D interrupted=base;
    assert(interrupted.fence(cutBefore,21,22)==-EIO);
    D recovered;assert(!recovered.open(cutBefore,d,21)&&recovered.eligible(201));
    assert(!base.fence(io,21,22));
    for(auto generation:{21u,22u}){
        D boot;Store cutAfter=io;
        assert(!boot.open(cutAfter,d,generation)&&boot.eligible(201)&&boot.eligible(204)&&!boot.eligible(206));
        // Re-enroll only after a live observation. An identity cannot inherit a preloaded row.
        assert(!boot.save(cutAfter,D::Row::copy(entry(206,7)))&&boot.eligible(206));
        D next;assert(!next.open(cutAfter,d,generation)&&next.eligible(206));
    }
    assert(!base.checkpoint(io,21)); // old side of the fence still exists
    printf("PASS favorite/recent and keyless eviction, two-journal recovery on either side, per-peer durable withdrawal and re-enrollment\n");
}
static void favoriteFlow(){
    using E=mt::NodeCache::Entry;using A=Owner<119>;
    A archive;Store io;A::Domain d{};d.node=OURS;d.epoch[0]=1;d.key[0]=2;
    assert(!archive.open(io,d,0));auto f=std::make_unique<Fixture>();Phone<119> h(f->s,archive);
    assert(!h.journal.load(h.journalStore,f->s.nodes,OURS));
    mt::companion::CompanionWorkspace workspace;mt::Companion phone(f->s,h,81,workspace,0);
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=77;
    input(phone,hello,1);for(unsigned i=0;i<100;i++)phone.poll(mt::Companion::WorkspaceProof{true});
    auto add=[&](uint32_t id,uint8_t pk){auto e=entry(id,pk);e.bytes[E::LocalFlags]=E::VerifiedSigner;
        archive.observe(e);assert(!archive.poll(io,0));};
    auto favorite=[&](uint32_t id,uint32_t req){
        meshtastic_AdminMessage admin{};admin.which_payload_variant=meshtastic_AdminMessage_set_favorite_node_tag;admin.set_favorite_node=id;
        meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
        auto &packet=request.packet;packet.id=req;packet.to=OURS;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
        packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;auto pb=pb_ostream_from_buffer(packet.decoded.payload.bytes,sizeof(packet.decoded.payload.bytes));
        assert(pb_encode(&pb,meshtastic_AdminMessage_fields,&admin));packet.decoded.payload.size=pb.bytes_written;
        const auto reads=io.reads;input(phone,request,22);assert(!results(h,req)&&io.reads==reads);
    };
    add(500,9);favorite(500,55);assert(!archive.poll(io,0));phone.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,55,0)==1);auto *e=f->s.nodes.get(f->s.nodes.find(500));
    assert(e&&e->favorite()&&!e->verifiedSigner()&&!e->bytes[E::KeyLength]);
    mt::NodeJournal reboot;mt::NodeCache recovered;assert(!reboot.load(h.journalStore,recovered,OURS));
    assert(recovered.find(500)>=0&&recovered.get(unsigned(recovered.find(500)))->favorite());
    h.pinnedNode=501;h.pin=3;add(501,3);favorite(501,56);assert(!archive.poll(io,0));phone.poll(mt::Companion::WorkspaceProof{true});
    e=f->s.nodes.get(f->s.nodes.find(501));assert(results(h,56,0)==1&&e&&e->bytes[E::KeyLength]==32&&!e->verifiedSigner());
    h.pinnedNode=502;add(502,5);favorite(502,57);assert(!archive.poll(io,0));phone.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,57,meshtastic_Routing_Error_BAD_REQUEST)==1&&f->s.nodes.find(502)<0);
    // RF admission can fail after an asynchronous directory read finishes.
    // Retain both the row and the selection revision for the next phone request.
    add(505,5);const auto rev=archive.revision();h.admission.busy=true;
    favorite(505,60);assert(!archive.poll(io,0));phone.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,60,meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED)==1&&archive.revision()==rev&&f->s.nodes.find(505)<0);
    h.admission.busy=false;archive.disconnect(0);
    favorite(505,61);assert(!archive.poll(io,0));phone.poll(mt::Companion::WorkspaceProof{true});
    assert(results(h,61,0)==1&&f->s.nodes.find(505)>=0);
    add(503,5);favorite(503,58);assert(!archive.poll(io,0));assert(!archive.forget(io,503));add(503,5);
    phone.poll(mt::Companion::WorkspaceProof{true});assert(results(h,58,meshtastic_Routing_Error_BAD_REQUEST)==1&&f->s.nodes.find(503)<0);
    add(504,5);favorite(504,59);assert(!archive.poll(io,0));input(phone,hello,100);phone.poll(mt::Companion::WorkspaceProof{true});
    assert(f->s.nodes.find(504)<0&&!results(h,59));
    printf("PASS local phone favorite and NodeJournal reboot, independent pin comparison, no signer upgrade, same-key removal and reconnect cancellation\n");
}
static Owner<119> *editArchive;
static Store *editFiles;
static int withdraw(uint32_t id,mt::NodeCache::Edit){
    return editArchive->forget(*editFiles,id);
}
static void editAdmission(){
    using Op=mt::NodeCache::Edit;
    auto f=std::make_unique<Fixture>();mt::NodeJournal journal;NodeStore backend;
    assert(!journal.load(backend,f->s.nodes,OURS));
    f->s.stop();f->radio.healthStatus=0;
    Owner<119> archive;Store io;Owner<119>::Domain d{};d.node=OURS;d.epoch[0]=1;d.key[0]=2;
    editArchive=&archive;editFiles=&io;
    assert(!archive.open(io,d,0));archive.observe(entry(699));assert(!archive.poll(io,0));
    const auto generation=archive.revision();
    auto edit=[&](Op op){return mt::NodeAdmin::edit(f->s,f->radio,journal,backend,699,op,2,false,nullptr,nullptr,withdraw);};
    auto reset=[&](){return mt::NodeAdmin::reset(f->s,f->radio,journal,backend,true,2,nullptr,
        [&](){return archive.clear(io);},withdraw);};
    for(unsigned i=0;i<mt::NodeCache::ProtectedLimit;++i){
        const auto e=entry(901+i);assert(!journal.ownerContact(backend,f->s.nodes,e,false,false,1,true));
    }
    f->radio.occupied=true;
    assert(edit(Op::Remove)==-EBUSY&&reset()==-EBUSY&&archive.revision()==generation);
    f->radio.occupied=false;
    assert(edit(Op::Ignore)==-ENOSPC&&archive.revision()==generation);
    mt::NodeCache::Entry recovered{};
    assert(archive.identity(0,699,recovered)==-EAGAIN);
    assert(!archive.poll(io,0)&&archive.identity(0,699,recovered)==1&&recovered.id()==699);
    assert(!edit(Op::Remove)&&archive.identity(0,699,recovered)==-ENOENT);
    archive.observe(entry(700));assert(!archive.poll(io,0));
    assert(!reset()&&archive.identity(0,700,recovered)==-ENOENT&&f->s.nodes.count()==mt::NodeCache::ProtectedLimit);
    editArchive=nullptr;editFiles=nullptr;
    puts("PASS busy reset/remove, protected-full ignore, archived peer retained and successful retries");
}
// Mirrors the production SX126x contract: stopForStorage is an RX-only
// transition and cannot be used to establish standby on an already-idle chip.
struct StandbyRadio final:Radio {
    unsigned rxStops=0;
    bool backlog=false;
    bool receiveEventPending()const override{return backlog;}
    int stopForStorage()override{
        ++rxStops;
        if(healthStatus!=1)return -EIO;
        healthStatus=0;return 0;
    }
    int restore()override{++restoreCalls;healthStatus=1;return 0;}
};
static void adminWhileStandby() {
    StandbyRadio radio;radio.healthStatus=0;
    RelayFixtureProvider enrolled(OURS);
    auto s=std::make_unique<mt::Service>(radio,false,OURS,81,true,false,&enrolled);
    assert(s->config.regionCode()==0&&!s->receiving()&&!s->endpointArmed());
    radio.backlog=true;
    assert(!s->beginPrivateAdministration()&&!s->faulted()&&enrolled.available&&!radio.rxStops);
    radio.backlog=false;radio.occupied=true;
    assert(!s->beginPrivateAdministration()&&!s->faulted()&&enrolled.available&&!radio.rxStops);
    radio.occupied=false;assert(s->beginPrivateAdministration()&&s->privateStoragePaused());
    NodeStore settings;auto journal=std::make_unique<mt::NodeJournal>();
    assert(!journal->load(settings,s->nodes,OURS));
    auto contact=entry(901);contact.bytes[mt::NodeCache::Entry::KeyLength]=0;
    memset(contact.bytes+mt::NodeCache::Entry::Key,0,32);
    assert(!journal->ownerContact(settings,s->nodes,contact,false,false,1,s->privateStoragePaused()));
    assert(s->endPrivateAdministration()&&!s->receiving()&&!s->endpointArmed());
    assert(s->nodes.get(0)->id()==901&&s->nodes.get(0)->favorite()&&enrolled.available);
    assert(!radio.rxStops&&!radio.restoreCalls&&!radio.sends);
    // An unexpected native receive state does not grant a storage lease.
    radio.healthStatus=1;
    assert(!s->beginPrivateAdministration()&&s->faulted()&&!enrolled.available&&!radio.rxStops);
    puts("PASS actual private/NodeJournal owner: enrolled UNSET idle, RX backlog, unchanged TX, retained keyless favorite and standby mismatch");
}
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
#include "../stock_protected_nodes/Files.h"
#include "pki/PkiCapacity.h"
static_assert(mt::pki::ContactLimit==16&&mt::pki::RuntimeSlots==4);
int main(){
    adminWhileStandby();
    auto f=std::make_unique<Fixture>();Owner<199> archive;Store thin;
    Owner<199>::Domain dom{};dom.node=OURS;dom.key[0]=1;dom.epoch[0]=2;
    assert(!archive.open(thin,dom,0));Phone<199> host(f->s,archive);Files volume;
    host.journal.files(volume);assert(!host.journal.load(host.journalStore,f->s.nodes,OURS));
    for(unsigned i=0;i<198;++i){
        const auto e=entry(1000+i);assert(f->s.nodes.accept(e,OURS,4,1)>=0);
        assert(!host.journal.edit(host.journalStore,f->s.nodes,e.id(),mt::NodeCache::Edit::Favorite,i,true));
    }
    assert(f->s.nodes.accept(entry(1500),OURS,4,1)==198);
    assert(host.journal.flush(host.journalStore,f->s.nodes,1,true,true)==1);
    // Cold boot, then backpressure/retry on each transport handshake.
    assert(!host.journal.load(host.journalStore,f->s.nodes,OURS));
    mt::companion::CompanionWorkspace ws;mt::Companion usb(f->s,host,81,ws,0);
    for(unsigned nonce:{69421u,237u}){
        host.out.clear();meshtastic_ToRadio req{};req.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
        req.want_config_id=nonce;input(usb,req,1);
        for(unsigned step=0;step<1200;++step){
            host.blocked=step%7==0;usb.poll(mt::Companion::WorkspaceProof{true});host.blocked=false;
            if(!host.out.empty()&&host.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)break;
        }
        assert(host.out.back().config_complete_id==nonce);
        bool count=false;std::set<uint32_t> peers;unsigned favorites=0;
        for(const auto &frame:host.out){
            if(frame.which_payload_variant==meshtastic_FromRadio_my_info_tag){count=true;assert(frame.my_info.nodedb_count==200);}
            if(frame.which_payload_variant!=meshtastic_FromRadio_node_info_tag)continue;
            const auto &node=frame.node_info;assert(peers.insert(node.num).second);
            if(node.num>=1000&&node.num<1198){assert(node.is_favorite&&node.has_user);++favorites;}
        }
        assert(count==(nonce!=69421)&&peers.size()==200&&favorites==198&&peers.count(OURS)&&peers.count(1500));
    }
    puts("PASS actual full/nodes-only PhoneAPI on cold XIAO: 200 distinct nodes, 198 favorites, high slots and backpressure");
}
#else
int main(){adminWhileStandby();directoryCase<119>();directoryCase<199>();durability();favoriteFlow();editAdmission();}
#endif
