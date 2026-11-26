// SPDX-License-Identifier: GPL-3.0-only
// Actual Position holder/cache/journal/Service/Companion/nanopb. Memory only;
// no physical radio, positioning sensor, external clock or cryptographic proof.
#include "../position_query_fixture.h"
#include "NodeJournal.h"
#include <array>
#include <climits>
#include <map>
#include <type_traits>
namespace {
using Cache=mt::NodeCache;using Entry=Cache::Entry;using Journal=mt::NodeJournal;
using Holder=mt::PeerPosition;using Bytes=std::vector<uint8_t>;
struct Store final:mt::SettingsBackend {
  std::map<std::string,Bytes> files;unsigned reads=0,writes=0;
  int read(const char *key,uint8_t *out,size_t capacity)override{
    ++reads;const auto it=files.find(key);if(it==files.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t size)override{
    ++writes;assert(size==Journal::RecordSize);files[key]=Bytes(data,data+size);return 0;
  }
};
Entry identity(uint32_t id=PEER){
  Entry e;e.identity(id,81,0);assert(Entry::text(e.bytes+Entry::Name,40,"Known peer",11));
  assert(Entry::text(e.bytes+Entry::Short,5,"KP",3));return e;
}
const Entry &at(const Cache &cache,uint32_t id=PEER){
  const int slot=cache.find(id);assert(slot>=0);return *cache.get(unsigned(slot));
}
void seed(Fixture &f,uint32_t id=PEER){assert(f.s.nodes.accept(identity(id),OURS,3,0)>=0);}
Holder holder(const meshtastic_Position &p){Holder h;assert(h.assign(p)&&h.present());return h;}
meshtastic_Position cached(const Cache &cache,uint32_t id=PEER){
  meshtastic_Position p{};at(cache,id).position.exportTo(p);return p;
}
void equal(const meshtastic_Position &a,const meshtastic_Position &b){assert(encodePosition(a)==encodePosition(b));}
void retained(const Cache &cache,const Holder &before,uint32_t id=PEER){assert(!memcmp(&at(cache,id).position,&before,sizeof(before)));}
void handshake(Fixture &f,uint8_t owner,const meshtastic_Position *expected){
  auto &host=owner?f.bh:f.h;host.out.clear();
  meshtastic_ToRadio q{};q.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;q.want_config_id=321;
  input(owner?f.bc:f.c,q,1500);unsigned peers=0,self=0,complete=0;
  for(const auto &out:host.out){
    if(out.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){assert(out.config_complete_id==321);++complete;}
    if(out.which_payload_variant!=meshtastic_FromRadio_node_info_tag)continue;
    if(out.node_info.num==OURS){++self;assert(!out.node_info.has_position);}
    if(out.node_info.num!=PEER)continue;
    ++peers;assert(!out.node_info.has_position);
    if(expected)equal(cached(f.s.nodes),*expected);
  }
  assert(peers==1&&self==1&&complete==1);
}
void layout_and_scalar_presence(){
  static_assert(std::is_trivially_copyable<Holder>::value);
  static_assert(sizeof(Holder)==87&&alignof(Holder)==1);
  static_assert(sizeof(Entry)==288&&alignof(Entry)==1);
  static_assert(sizeof(Cache)==4676&&sizeof(Journal)==4700);
  static_assert(Entry::Size==92&&Journal::RecordSize==1492);
  static_assert(offsetof(Entry,lastHeardBytes)==95&&offsetof(Entry,deviceMetrics)==99&&offsetof(Entry,position)==120);
  printf("SIZES holder=%zu entry=%zu cache=%zu journal=%zu wire_entry=%u wire_record=%zu\n",
         sizeof(Holder),sizeof(Entry),sizeof(Cache),sizeof(Journal),unsigned(Entry::Size),Journal::RecordSize);
  Holder empty;meshtastic_Position p=fullPosition();empty.exportTo(p);assert(!empty.present());equal(p,meshtastic_Position{});
  // The required coordinate pair and every subset of five other optional
  // fields retain their presence, including explicitly present numeric zero.
  for(unsigned mask=0;mask<32;++mask){
    p=fullPosition();p.has_altitude=mask&1;p.has_altitude_hae=mask&2;p.has_altitude_geoidal_separation=mask&4;
    p.has_ground_speed=mask&8;p.has_ground_track=mask&16;
    p.latitude_i=p.longitude_i=p.altitude=p.altitude_hae=p.altitude_geoidal_separation=0;
    p.ground_speed=p.ground_track=0;Holder h=holder(p);meshtastic_Position out=fullPosition();h.exportTo(out);equal(out,p);
    assert(out.has_altitude==p.has_altitude&&out.has_altitude_hae==p.has_altitude_hae&&
      out.has_altitude_geoidal_separation==p.has_altitude_geoidal_separation&&
      out.has_ground_speed==p.has_ground_speed&&out.has_ground_track==p.has_ground_track);
    assert(out.altitude==0&&out.altitude_hae==0&&out.altitude_geoidal_separation==0&&out.ground_speed==0&&out.ground_track==0);
  }
  p=fullPosition();p.latitude_i=-900000000;p.longitude_i=1800000000;p.altitude=INT32_MIN;
  p.timestamp_millis_adjust=INT32_MIN;p.altitude_hae=INT32_MAX;p.altitude_geoidal_separation=INT32_MIN;
  p.time=p.timestamp=p.PDOP=p.HDOP=p.VDOP=p.gps_accuracy=UINT32_MAX;
  p.ground_speed=p.ground_track=p.fix_quality=p.fix_type=p.sats_in_view=UINT32_MAX;
  p.sensor_id=p.next_update=p.seq_number=UINT32_MAX;p.precision_bits=32;
  Holder h=holder(p);meshtastic_Position out;h.exportTo(out);equal(out,p);
  p=meshtastic_Position{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=900000000;p.longitude_i=-1800000000;
  assert(h.assign(p));h.exportTo(out);equal(out,p); // Whole sparse replacement clears old optional values.
}
void invalid_atomic_and_admission(){
  Holder h=holder(fullPosition());const Holder original=h;
  for(unsigned mode=0;mode<8;++mode){
    auto p=fullPosition();
    if(mode==0)p.has_latitude_i=false;
    if(mode==1)p.has_longitude_i=false;
    if(mode==2)p.latitude_i=900000001;
    if(mode==3)p.latitude_i=-900000001;
    if(mode==4)p.longitude_i=1800000001;
    if(mode==5)p.longitude_i=-1800000001;
    if(mode==6)p.precision_bits=33;
    if(mode==7){using Enum=decltype(p.location_source);const Enum invalid=static_cast<Enum>(4);memcpy(&p.location_source,&invalid,sizeof(invalid));}
    assert(!h.assign(p)&&!memcmp(&h,&original,sizeof(h)));
  }
  auto p=fullPosition();using Enum=decltype(p.altitude_source);const Enum invalid=static_cast<Enum>(5);
  memcpy(&p.altitude_source,&invalid,sizeof(invalid));assert(!h.assign(p)&&!memcmp(&h,&original,sizeof(h)));
  Cache cache;Store backend;Journal journal;assert(!journal.load(backend,cache,OURS));
  assert(cache.accept(identity(),OURS,3,0)==0);assert(journal.flush(backend,cache,0,true,true)==1);
  const Entry before=at(cache);const unsigned writes=backend.writes;
  assert(cache.observePosition(PEER,h));assert(!cache.dirty()&&cache.count()==1&&backend.writes==writes);
  assert(!memcmp(before.bytes,at(cache).bytes,Entry::Size));
  for(uint32_t id:{0u,1u,OURS,3u,0xfffffffeu,UINT32_MAX}){const Cache prior=cache;assert(!cache.observePosition(id,h)&&!memcmp(&prior,&cache,sizeof(cache)));}
  Holder empty;const Cache prior=cache;assert(!cache.observePosition(PEER,empty)&&!memcmp(&prior,&cache,sizeof(cache)));
  assert(cache.edit(PEER,OURS,Cache::Edit::Ignore)==1);assert(!cache.observePosition(PEER,h)&&!at(cache).position.present());
  assert(cache.edit(PEER,OURS,Cache::Edit::Unignore)==1);assert(cache.observePosition(PEER,h));
  assert(cache.edit(3,OURS,Cache::Edit::Ignore)==1&&cache.edit(3,OURS,Cache::Edit::Unignore)==1);
  assert(at(cache,3).placeholder()&&!cache.observePosition(3,h)&&!at(cache,3).position.present());
  Cache frozen;Journal failed;assert(failed.load(backend,frozen,1)==-EINVAL&&!frozen.ready());
  const Cache previous=frozen;assert(!frozen.observePosition(PEER,h)&&!memcmp(&frozen,&previous,sizeof(frozen)));
}
void journal_and_slot_lifecycle(){
  Cache a,b;Store one,two;Journal j1,j2;assert(!j1.load(one,a,OURS)&&!j2.load(two,b,OURS));
  for(unsigned i=0;i<Cache::Capacity;++i){
    const uint32_t id=10+i;assert(a.accept(identity(id),OURS,3,1)>=0&&b.accept(identity(id),OURS,3,1)>=0);
    auto p=fullPosition();p.seq_number=id;assert(b.observePosition(id,holder(p)));
  }
  assert(j1.flush(one,a,0,true,true)==1&&j2.flush(two,b,0,true,true)==1&&one.files==two.files);
  const auto stored=two.files;assert(!b.dirty());
  for(unsigned i=0;i<Cache::Capacity;++i)assert(!memcmp(two.files["mtbase/nodes0"].data()+Journal::HeaderSize+i*Entry::Size,b.get(i)->bytes,Entry::Size));
  assert(b.edit(10,OURS,Cache::Edit::Favorite)==1);Entry replacement=identity(99);replacement.position=holder(fullPosition());
  assert(b.accept(replacement,OURS,0,0)==1);
  assert(b.find(11)<0&&!at(b,99).position.present()&&at(b,10).position.present());
  assert(b.edit(12,OURS,Cache::Edit::Remove)==1);assert(b.find(12)<0&&at(b,13).position.present());
  assert(two.files==stored); // No storage operation follows volatile observations/ordinary cache edits.
  Journal reboot;assert(!reboot.load(two,b,OURS)&&b.count()==Cache::Capacity&&!b.dirty());
  for(unsigned i=0;i<b.count();++i)assert(!b.get(i)->position.present());
  assert(one.files==two.files&&Entry::Size==92&&Journal::RecordSize==1492);
}
void matched_replies_and_exports(){
  for(uint8_t model:{81,127})for(uint8_t owner:{0,1}){
    Fixture f(true,model);seed(f);handshake(f,owner,nullptr);const auto before=at(f.s.nodes);
    beginQuery(f,100,owner);f.rx(goodFrame(),500);equal(cached(f.s.nodes),fullPosition());
    assert(!memcmp(before.bytes,at(f.s.nodes).bytes,Entry::Size)&&f.s.nodes.count()==1);
    const auto expected=fullPosition();handshake(f,0,&expected);handshake(f,1,&expected);
  }
}
void ordinary_announcements_and_bad_payloads(){
  for(uint8_t model:{81,127})for(bool broadcast:{false,true}){
    Fixture f(true,model);seed(f);const auto config=f.s.config;
    f.rx(rawFrame(encodeData(encodePosition(fullPosition()),0),PEER,broadcast?0xffffffffu:OURS),500);
    equal(cached(f.s.nodes),fullPosition());assert(f.radio.sends==0&&f.s.config.same(config));
    const auto expected=fullPosition();handshake(f,0,&expected);handshake(f,1,&expected);
    const Holder before=at(f.s.nodes).position;
    auto changed=fullPosition();changed.seq_number=8;
    f.rx(rawFrame(encodeData(encodePosition(changed),0),PEER,broadcast?0xffffffffu:OURS),600);retained(f.s.nodes,before);
  }
  for(unsigned mode=0;mode<6;++mode){
    Fixture f;seed(f);assert(f.s.nodes.observePosition(PEER,holder(fullPosition())));const Holder before=at(f.s.nodes).position;
    auto payload=encodePosition(fullPosition());
    if(mode==0)payload.pop_back();
    if(mode==1)payload.insert(payload.end(),{0xf8,1,1});
    if(mode==2)payload.insert(payload.end(),{0x0d,0,0,0,0}); // Duplicate latitude field.
    if(mode==3){auto p=fullPosition();p.latitude_i=900000001;payload=encodePosition(p);}
    if(mode==4){auto p=fullPosition();p.has_longitude_i=false;payload=encodePosition(p);}
    if(mode==5)payload.clear();
    f.rx(rawFrame(encodeData(payload,0)),500);retained(f.s.nodes,before);
  }
}
void unknown_placeholder_and_ignored(){
  Fixture unknown;unknown.rx(rawFrame(encodeData(encodePosition(fullPosition()),0)),500);assert(!unknown.s.nodes.count());
  Fixture placeholder;assert(placeholder.s.nodes.edit(PEER,OURS,Cache::Edit::Ignore)==1);
  assert(placeholder.s.nodes.edit(PEER,OURS,Cache::Edit::Unignore)==1);
  placeholder.rx(rawFrame(encodeData(encodePosition(fullPosition()),0)),500);
  assert(at(placeholder.s.nodes).placeholder()&&!at(placeholder.s.nodes).position.present());
  Fixture ignored;seed(ignored);assert(ignored.s.nodes.observePosition(PEER,holder(fullPosition())));
  assert(ignored.s.nodes.edit(PEER,OURS,Cache::Edit::Ignore)==1);
  auto changed=fullPosition();changed.seq_number=8;
  ignored.rx(rawFrame(encodeData(encodePosition(changed),0)),500);assert(!at(ignored.s.nodes).position.present());
}
void correlation_and_immutable_snapshot(){
  for(unsigned mode=0;mode<6;++mode){
    Fixture f;seed(f);assert(f.s.nodes.observePosition(PEER,holder(fullPosition())));const Holder before=at(f.s.nodes).position;
    beginQuery(f);auto p=fullPosition();p.seq_number=8;auto wire=rawFrame(encodeData(encodePosition(p)));
    if(mode==0)wire=rawFrame(encodeData(encodePosition(p),78));
    if(mode==1)wire=rawFrame(encodeData(encodePosition(p)),0x98765432);
    if(mode==2)wire=rawFrame(encodeData(encodePosition(p)),PEER,0xffffffffu);
    if(mode==3)wire[13]^=1;
    if(mode==4)wire[12]=1;
    if(mode==5)wire=rawFrame(encodeData(encodePosition(p),77,3,true));
    f.rx(wire,500);retained(f.s.nodes,before);
  }
  for(uint8_t model:{81,127})for(uint8_t owner:{0,1}){
    Fixture f(true,model);seed(f);beginQuery(f,100,owner);
    const auto approved=fullPosition();const auto body=encodePosition(approved);const auto a=rawFrame(encodeData(body));
    f.s.rx(a.data(),a.size(),3,500,700);
    auto different=fullPosition();different.seq_number=1;const auto b=rawFrame(encodeData(encodePosition(different)));
    (owner?f.bc:f.c).receive(b.data(),b.size(),-90,3,500,mt::Companion::WorkspaceProof{true});
    std::vector<uint8_t> forwarded;assert(positions(owner?f.bh:f.h,77,&forwarded)==1&&forwarded==body);
    equal(cached(f.s.nodes),approved);handshake(f,owner,&approved);
  }
}
}
int main(){
  (void)&results;
  const struct {const char *name;void(*run)();} tests[]={
    {"compact layout all32 optional masks zero extrema sparse replacement",layout_and_scalar_presence},
    {"atomic invalid values known-peer ignore placeholder frozen admission",invalid_atomic_and_admission},
    {"journal bytes unchanged RAM reset eviction removal lifecycle",journal_and_slot_lifecycle},
    {"matched Public replies both models and owners fresh NodeInfo exports",matched_replies_and_exports},
    {"canonical announcements dedup bad payloads no local mutation",ordinary_announcements_and_bad_payloads},
    {"unknown placeholder ignored peers never gain Position",unknown_placeholder_and_ignored},
    {"correlation and immutable approved snapshot across split input",correlation_and_immutable_snapshot}};
  unsigned groups=0;for(const auto &test:tests){test.run();group(test.name);++groups;}
  std::cout<<"PEER_POSITION_GROUPS "<<groups<<"\n";
}
