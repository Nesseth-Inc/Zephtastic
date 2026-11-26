// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Endpoint/Companion/nanopb; deterministic in-memory radio only.
#include "../telemetry_query_fixture.h"
#include <limits>
namespace {
using Entry=mt::NodeCache::Entry;
void seed(Fixture &f,uint32_t id=PEER){
  Entry e{};e.identity(id,81,0);
  assert(Entry::text(e.bytes+Entry::Name,40,"Known peer",11));
  assert(Entry::text(e.bytes+Entry::Short,5,"KP",3));
  assert(f.s.nodes.accept(e,OURS,3,0)>=0);
}
const Entry &entry(const Fixture &f,uint32_t id=PEER){
  const int slot=f.s.nodes.find(id);assert(slot>=0);return *f.s.nodes.get(unsigned(slot));
}
void equalMetrics(const meshtastic_DeviceMetrics &a,const meshtastic_DeviceMetrics &b){
  assert(a.has_battery_level==b.has_battery_level&&a.has_voltage==b.has_voltage&&
    a.has_channel_utilization==b.has_channel_utilization&&a.has_air_util_tx==b.has_air_util_tx&&
    a.has_uptime_seconds==b.has_uptime_seconds);
  if(a.has_battery_level)assert(a.battery_level==b.battery_level);
  if(a.has_voltage)assert(!memcmp(&a.voltage,&b.voltage,4));
  if(a.has_channel_utilization)assert(!memcmp(&a.channel_utilization,&b.channel_utilization,4));
  if(a.has_air_util_tx)assert(!memcmp(&a.air_util_tx,&b.air_util_tx,4));
  if(a.has_uptime_seconds)assert(a.uptime_seconds==b.uptime_seconds);
}
meshtastic_DeviceMetrics cached(const Fixture &f){
  meshtastic_DeviceMetrics m{};entry(f).deviceMetrics.exportTo(m);return m;
}
void handshake(Fixture &f,uint8_t owner,const meshtastic_DeviceMetrics *expected){
  auto &host=owner?f.bh:f.h;host.out.clear();
  meshtastic_ToRadio q{};q.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;q.want_config_id=321;
  input(owner?f.bc:f.c,q,1500);unsigned peers=0,self=0,complete=0;
  for(const auto &out:host.out){
    if(out.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){assert(out.config_complete_id==321);++complete;}
    if(out.which_payload_variant!=meshtastic_FromRadio_node_info_tag)continue;
    if(out.node_info.num==OURS){++self;assert(!out.node_info.has_device_metrics);}
    if(out.node_info.num!=PEER)continue;
    ++peers;assert(!out.node_info.has_device_metrics);
    if(expected)equalMetrics(cached(f),*expected);
  }
  assert(peers==1&&self==1&&complete==1);
}
void prime(Fixture &f){
  seed(f);mt::PeerDeviceMetrics m;assert(m.assign(fullTelemetry().variant.device_metrics));
  assert(f.s.nodes.observeDeviceMetrics(PEER,m));
}
void retained(const Fixture &f,const mt::PeerDeviceMetrics &before){assert(!memcmp(&entry(f).deviceMetrics,&before,sizeof(before)));}
}
int main(){
  (void)&results;(void)&telemetries;(void)&goodFrame;
  unsigned groups=0;
  for(uint8_t model:{81,127})for(uint8_t owner:{0,1})for(unsigned mask=1;mask<32;++mask){
    Fixture f(true,model);seed(f);handshake(f,owner,nullptr);
    auto t=fullTelemetry();auto &m=t.variant.device_metrics;
    m.has_battery_level=mask&1;m.has_voltage=mask&2;m.has_channel_utilization=mask&4;m.has_air_util_tx=mask&8;m.has_uptime_seconds=mask&16;
    m.battery_level=0;m.voltage=-0.0f;m.channel_utilization=0.0f;m.air_util_tx=0.0f;m.uptime_seconds=0;
    const auto durable=entry(f);const bool dirty=f.s.nodes.dirty();beginQuery(f,100,owner);
    f.rx(rawFrame(encodeData(encodeTelemetry(t))),500);equalMetrics(cached(f),m);
    assert(!memcmp(durable.bytes,entry(f).bytes,Entry::Size)&&f.s.nodes.dirty()==dirty&&f.s.nodes.count()==1);
    handshake(f,0,&m);handshake(f,1,&m);assert(f.radio.sends==1);
  }
  group("both models/owners/all31 masks: matched replies retain optional zeros and reconnect emits thin peer NodeInfo");++groups;
  {
    Fixture f;seed(f);beginQuery(f);auto t=fullTelemetry();f.rx(rawFrame(encodeData(encodeTelemetry(t))),500);equalMetrics(cached(f),t.variant.device_metrics);
    auto q=query(78);input(f.c,q,600);f.s.poll(800,0);assert(f.radio.sends==2);f.done(801);
    meshtastic_Telemetry sparse{};sparse.which_variant=meshtastic_Telemetry_device_metrics_tag;
    sparse.variant.device_metrics.has_uptime_seconds=true;sparse.variant.device_metrics.uptime_seconds=0;
    f.rx(rawFrame(encodeData(encodeTelemetry(sparse),78),PEER,OURS,701),1000);
    equalMetrics(cached(f),sparse.variant.device_metrics);handshake(f,0,&sparse.variant.device_metrics);handshake(f,1,&sparse.variant.device_metrics);
  }
  group("second sparse matched snapshot clears old fields and preserves present zero on both fresh exports");++groups;
  for(unsigned mode=0;mode<10;++mode){
    Fixture f;prime(f);const auto before=entry(f).deviceMetrics;beginQuery(f);
    auto payload=encodeTelemetry(fullTelemetry());auto wire=goodFrame();
    if(mode==0)wire=goodFrame(78);
    if(mode==1)wire=rawFrame(encodeData(payload),0x98765432);
    if(mode==2)wire=rawFrame(encodeData(payload),PEER,0xffffffffu);
    if(mode==3)wire[13]^=1;
    if(mode==4)wire[12]=1;
    if(mode==5)wire=rawFrame(encodeData(payload,77,67,true));
    if(mode==6){payload.push_back(8);payload.push_back(0);wire=rawFrame(encodeData(payload));}
    if(mode==7){auto t=fullTelemetry();t.variant.device_metrics.voltage=-1;wire=rawFrame(encodeData(encodeTelemetry(t)));}
    if(mode==8){auto t=fullTelemetry();t.which_variant=meshtastic_Telemetry_environment_metrics_tag;t.variant={};wire=rawFrame(encodeData(encodeTelemetry(t)));}
    if(mode==9){meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;wire=rawFrame(encodeData(encodeTelemetry(t)));}
    f.rx(wire,500);retained(f,before);
    f.rx(goodFrame(77,702),600);equalMetrics(cached(f),fullTelemetry().variant.device_metrics);
  }
  group("wrong correlation/channel/header and malformed/noncanonical/invalid/empty telemetry never alter cache");++groups;
  {
    Fixture f;prime(f);const auto before=entry(f).deviceMetrics;
    f.rx(goodFrame(),500);retained(f,before);
    f.rx(rawFrame(encodeData(encodeTelemetry(fullTelemetry()),0)),600);retained(f,before);
    auto broadcast=fullTelemetry();broadcast.variant.device_metrics.uptime_seconds=12345;
    f.rx(rawFrame(encodeData(encodeTelemetry(broadcast),0),PEER,0xffffffffu,701),700);equalMetrics(cached(f),broadcast.variant.device_metrics);
    assert(f.radio.sends==0);
  }
  group("unmatched replies and local unicast preserve cache, admitted broadcast supplies current device metrics");++groups;
  {
    Fixture f;beginQuery(f);f.rx(goodFrame(),500);assert(!f.s.nodes.count()&&telemetries(f.h)==1);
    Fixture p;assert(p.s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Ignore)==1);
    assert(p.s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Unignore)==1);beginQuery(p);p.rx(goodFrame(),500);
    assert(entry(p).placeholder()&&!entry(p).deviceMetrics.present()&&telemetries(p.h)==1);
    Fixture ignored;prime(ignored);beginQuery(ignored);
    assert(ignored.s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Ignore)==1);ignored.rx(goodFrame(),500);assert(!entry(ignored).deviceMetrics.present());assert(!telemetries(ignored.h));
  }
  group("unknown/placeholder peers remain deliverable without allocation; ignored peer cannot refresh");++groups;
  {
    Fixture f;prime(f);const auto before=entry(f).deviceMetrics;beginQuery(f);f.c.resetStream();f.rx(goodFrame(),500);retained(f,before);assert(!telemetries(f.h));
    Fixture shared;seed(shared);input(shared.c,query(),100);input(shared.bc,query(),100);shared.s.poll(300,0);shared.done(301);shared.c.resetStream();shared.rx(goodFrame(),500);
    equalMetrics(cached(shared),fullTelemetry().variant.device_metrics);assert(!telemetries(shared.h)&&telemetries(shared.bh)==1);
    const auto expected=fullTelemetry().variant.device_metrics;handshake(shared,0,&expected);
  }
  group("disconnect cancels ownership; remaining shared owner updates cache for a later handshake");++groups;
  {
    Fixture f;seed(f);beginQuery(f);auto wire=goodFrame();f.rx(wire,500);const auto before=entry(f).deviceMetrics;
    auto changed=fullTelemetry();changed.variant.device_metrics.uptime_seconds=12;
    f.rx(rawFrame(encodeData(encodeTelemetry(changed))),600);retained(f,before);
    for(const auto &out:f.h.out)assert(out.which_payload_variant!=meshtastic_FromRadio_node_info_tag);
    const auto expected=fullTelemetry().variant.device_metrics;handshake(f,0,&expected);
  }
  group("consumed/duplicate approval cannot refresh metrics and receive adds no new NodeInfo event");++groups;
  {
    mt::PeerDeviceMetrics holder;auto t=fullTelemetry();auto body=encodeTelemetry(t);
    assert(mt::TelemetryQuery::cachedMetrics(body.data(),body.size(),holder));const auto before=holder;
    for(unsigned mode=0;mode<5;++mode){
      auto bad=body;
      if(mode==0)bad.pop_back();
      if(mode==1)bad.insert(bad.end(),{0xf8,1,1});
      if(mode==2)bad.insert(bad.end(),body.begin(),body.end());
      if(mode==3)bad.insert(bad.end(),{8,0});
      if(mode==4){auto invalid=t;invalid.variant.device_metrics.air_util_tx=std::numeric_limits<float>::infinity();bad=encodeTelemetry(invalid);}
      assert(!mt::TelemetryQuery::cachedMetrics(bad.data(),bad.size(),holder)&&!memcmp(&holder,&before,sizeof(holder)));
    }
  }
  group("actual canonical Telemetry decoder preserves holder for every invalid cache snapshot");++groups;
  for(uint8_t model:{81,127})for(uint8_t owner:{0,1}){
    Fixture f(true,model);seed(f);beginQuery(f,100,owner);
    const auto approved=fullTelemetry();const auto body=encodeTelemetry(approved);
    const auto a=rawFrame(encodeData(body));
    // Preserve the real Service approval while changing only the transport's
    // current canonical payload at the Service/Companion API boundary.
    f.s.rx(a.data(),a.size(),3,500,700);
    meshtastic_Telemetry different{};different.which_variant=meshtastic_Telemetry_device_metrics_tag;
    different.variant.device_metrics.has_uptime_seconds=true;different.variant.device_metrics.uptime_seconds=1;
    const auto b=rawFrame(encodeData(encodeTelemetry(different)));
    (owner?f.bc:f.c).receive(b.data(),b.size(),-90,3,500,mt::Companion::WorkspaceProof{true});
    std::vector<uint8_t> forwarded;
    assert(telemetries(owner?f.bh:f.h,77,&forwarded)==1&&forwarded==body);
    equalMetrics(cached(f),approved.variant.device_metrics);
    handshake(f,owner,&approved.variant.device_metrics);assert(f.radio.sends==1);
  }
  group("cache and forwarding both consume the immutable Service-approved snapshot across split input");++groups;
  assert(groups==9);std::cout<<"METRICS_EXPORT_GROUPS "<<groups<<"\n";
}
