// SPDX-License-Identifier: GPL-3.0-only
#include "Service.h"
#include "stock/position/PositionPrecision.h"
#include "SettingsCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
struct Memory:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> files;unsigned writes=0;bool fail=false;
  int read(const char *k,uint8_t*p,size_t n)override{auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(fail)return -EIO;files[k]={p,p+n};return 0;}
};
struct Radio:mt::Radio{
  int state=0;bool occupied=false;unsigned sends=0;std::vector<uint8_t> sent;
  bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return occupied;}
  uint32_t airtime(size_t)override{return 100;}int send(uint8_t *p,size_t n)override{++sends;sent.assign(p,p+n);return 0;}bool completion(int&)override{return false;}
};
struct HotObservation {unsigned calls=0;uint32_t from=0,utc=0,uptime=0;bool viaMqtt=false;
  static void receive(void *ctx,uint32_t node,uint32_t epoch,uint32_t seconds,bool mqtt){auto &s=*static_cast<HotObservation*>(ctx);++s.calls;s.from=node;s.utc=epoch;s.uptime=seconds;s.viaMqtt=mqtt;}
};
static void checkPositionRelay(const mt::Config &config, unsigned channel, const meshtastic_Position &pos, bool allowed, bool enableManager = true)
{
  constexpr uint32_t self = 0x12345678, peer = 0x24680135;
  Radio radio;
  mt::test::KnownChannelFixtureClock clock;
  mt::Service service(radio, true, self, 127, true, false);
  clock.bind(service);
  assert(service.apply(config));
  assert(service.start(false, 0));
  assert(service.armEndpoint(true) && service.relaying());
  mt::modules::TrafficManagementWindow traffic;
  mt::modules::TrafficManagementSettings settings;
  traffic.configure(settings);
  if (enableManager) service.trafficManagement(&traffic, 0);
  meshtastic_Data d{};
  d.portnum = meshtastic_PortNum_POSITION_APP;
  d.has_bitfield = true;
  auto nested = pb_ostream_from_buffer(d.payload.bytes, sizeof(d.payload.bytes));
  assert(pb_encode(&nested, meshtastic_Position_fields, &pos));
  d.payload.size = nested.bytes_written;
  uint8_t data[239], frame[255];
  auto wire = pb_ostream_from_buffer(data, sizeof(data));
  assert(pb_encode(&wire, meshtastic_Data_fields, &d));
  mt::PrimaryChannel::Selection key{};
  assert(config.channelSelection(channel, key));
  bench::MtCodec sender;
  assert(sender.setKey(key.hash, key.bytes, key.length));
  mt::PrimaryChannel::clear(key);
  size_t size=0;
  assert(sender.encodeZeroHopData(peer, UINT32_MAX, 391, data, wire.bytes_written, frame, sizeof(frame), size));
  frame[12] = (3 << 5) | 3;
  bool accepted = false;
  service.rx(frame, size, -5, 100, 0, true, &accepted);
  assert(accepted == allowed && service.trafficSuppressed() == !allowed);
  for (uint32_t now = 200; now < 10000 && !radio.sends; now += 100) service.poll(now, 0);
  assert(radio.sends == unsigned(allowed));
  if (allowed) {
    assert(radio.sent.size() == size && !memcmp(frame, radio.sent.data(), 12));
    assert(!memcmp(frame + 16, radio.sent.data() + 16, size - 16));
  }
}

static void positionRelayCases()
{
  mt::Config cfg;
  cfg.bytes[4] = 1;
  auto node = cfg.node(); node.setPublic(); cfg.node(node);
  assert(cfg.setDeviceRole(meshtastic_Config_DeviceConfig_Role_ROUTER, 0));
  cfg.setTxEnabled(true);
  assert(!cfg.positionPotential() && cfg.relayPositionPrecision(0) == 32);
  meshtastic_Position pos{};
  pos.has_latitude_i = pos.has_longitude_i = true;
  pos.latitude_i = 123456789; pos.longitude_i = -23456789; pos.precision_bits = 32;
  checkPositionRelay(cfg, 0, pos, true); // Stock skips an unset channel limit.
  mt::GnssSettings gnss = cfg.gnssSettings(); gnss.enabled = true;
  assert(cfg.setGnssSettings(gnss)); // Receiver enabled; no fix or fixed position.
  assert(cfg.setPrimaryPositionPrecision(24) && cfg.relayPositionPrecision(0) == 15);
  checkPositionRelay(cfg, 0, pos, true, false);
  checkPositionRelay(cfg, 0, pos, false);
  pos.precision_bits = 0; checkPositionRelay(cfg, 0, pos, false);
  pos.precision_bits = 15; checkPositionRelay(cfg, 0, pos, false);
  pos.latitude_i = mt::stock::truncateCoordinate(pos.latitude_i, 15u);
  pos.longitude_i = mt::stock::truncateCoordinate(pos.longitude_i, 15u);
  checkPositionRelay(cfg, 0, pos, true);
  pos.precision_bits = 12;
  pos.latitude_i = mt::stock::truncateCoordinate(pos.latitude_i, 12u);
  pos.longitude_i = mt::stock::truncateCoordinate(pos.longitude_i, 12u);
  checkPositionRelay(cfg, 0, pos, true);
  pos = meshtastic_Position_init_zero; pos.time = 123;
  checkPositionRelay(cfg, 0, pos, true);

  mt::ChannelRecord secondary;
  assert(secondary.set(2, "Local", nullptr, 0, 12) && cfg.setSecondaryChannel(1, secondary));
  pos.has_latitude_i = true; pos.latitude_i = 123456789; pos.precision_bits = 32;
  checkPositionRelay(cfg, 1, pos, true); // Custom name.
  assert(secondary.set(2, "MediumFast", nullptr, 0, 12) && cfg.setSecondaryChannel(2, secondary));
  assert(cfg.relayPositionPrecision(2) == 12);
  checkPositionRelay(cfg, 2, pos, false);
  pos.precision_bits = 12; pos.latitude_i = mt::stock::truncateCoordinate(pos.latitude_i, 12u);
  checkPositionRelay(cfg, 2, pos, true);
  assert(secondary.set(2, "MediumFast", nullptr, 0, 10) && cfg.setSecondaryChannel(3, secondary));
  checkPositionRelay(cfg, 2, pos, false); // Same on-air identity, stricter slot.
  const uint8_t privateKey[16] = {7};
  assert(secondary.set(2, "LongFast", privateKey, sizeof(privateKey), 18));
  assert(cfg.setSecondaryChannel(4, secondary) && cfg.relayPositionPrecision(4) == 32);
  // The primary journal expands non-default public aliases before persisting.
  mt::Config aliasConfig = cfg;
  const uint8_t alias = 2;
  mt::PrimaryChannel::Selection selected{};
  assert(mt::PrimaryChannel::normalize(&alias, 1, selected, cfg.effectivePrimaryName()));
  auto aliasNode = aliasConfig.node();
  aliasNode.setKey(selected.hash, selected.bytes, selected.length);
  mt::PrimaryChannel::clear(selected);
  aliasConfig.node(aliasNode);
  assert(aliasConfig.valid() && aliasConfig.relayPositionPrecision(0) == 15);
  assert(cfg.enableLicensedOperation() && cfg.relayPositionPrecision(0) == 32);
  assert(cfg.relayPositionPrecision(2) == 32);
}
#include "neighbor_relay_cases.h"
int main(){
  neighborRelayCases();
  positionRelayCases();
  constexpr uint32_t self=0x12345678,peer=0x24680135;
  Radio radio;mt::test::KnownChannelFixtureClock clock;mt::Service service(radio,true,self,127,true,false);clock.bind(service);
  HotObservation hot;assert(service.bindHotNodeObservationSink(HotObservation::receive,&hot));
  mt::Config c;c.bytes[4]=1;auto r=c.node();r.setPublic();c.node(r);assert(service.apply(c)&&service.start(false,0)&&service.armEndpoint(true));
  mt::modules::TrafficManagementSettings settings;meshtastic_ModuleConfig_TrafficManagementConfig cfg{};cfg.rate_limit_window_secs=300;cfg.rate_limit_max_packets=1;cfg.unknown_packet_threshold=2;settings.set(cfg);
  mt::modules::TrafficManagementWindow window;window.configure(settings);service.trafficManagement(&window,0);
  auto receive=[&](uint32_t from,uint32_t to,uint32_t id,unsigned port,bool opaque=false){
    meshtastic_Data d{};d.has_bitfield=true;d.portnum=static_cast<meshtastic_PortNum>(port);d.payload.size=1;d.payload.bytes[0]='x';
    if(port==meshtastic_PortNum_ROUTING_APP){d.payload.size=2;d.payload.bytes[0]=0x18;d.payload.bytes[1]=0;}
    uint8_t data[239],frame[255];auto stream=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&stream,meshtastic_Data_fields,&d));size_t size=0;
    assert(service.codec.encodeZeroHopData(from,to,id,data,stream.bytes_written,frame,sizeof(frame),size));
    if(opaque)++frame[13];
    bool delivered=false;service.rx(frame,size,0,10,123,true,nullptr,false,1792660000u,10,0,0,&delivered);
    if(from==self)assert(!delivered);
    return service.trafficSuppressed();
  };
  for(unsigned i=0;i<4;++i)assert(!receive(self,UINT32_MAX,42+i,1,i>1));
  assert(!hot.calls&&!service.route.pending());
  assert(!receive(peer,UINT32_MAX,1,1));assert(hot.calls==1&&hot.from==peer&&hot.utc==1792660000u&&hot.uptime==10&&!hot.viaMqtt);
  assert(receive(peer,UINT32_MAX,2,1));assert(hot.calls==2); // A traffic-suppressed decoded packet still updates node metadata.
  assert(!receive(peer,UINT32_MAX,3,meshtastic_PortNum_ROUTING_APP));
  assert(!receive(peer,UINT32_MAX,4,meshtastic_PortNum_ADMIN_APP));
  assert(!receive(peer,self,5,1));
  service.trafficManagement(&window,80ull*60000);assert(!receive(peer,UINT32_MAX,6,1));
  assert(!receive(peer+1,UINT32_MAX,7,1,true));assert(!receive(peer+1,UINT32_MAX,8,1,true));assert(receive(peer+1,UINT32_MAX,9,1,true));
  service.trafficManagement(&window,96ull*60000);assert(!receive(peer+1,UINT32_MAX,10,1,true));
  service.trafficManagement(nullptr,96ull*60000);assert(!receive(peer,UINT32_MAX,11,1));assert(!radio.sends);
  service.trafficManagement(&window,97ull*60000);const unsigned beforeTelemetry=hot.calls;
  assert(!receive(peer+2,UINT32_MAX,12,meshtastic_PortNum_TELEMETRY_APP));
  assert(receive(peer+2,UINT32_MAX,13,meshtastic_PortNum_TELEMETRY_APP));
  assert(hot.calls==beforeTelemetry+2); // Both telemetry receptions are observed before rate suppression.
  uint8_t same[255]{};size_t sameSize=0;meshtastic_Data duplicate{};duplicate.has_bitfield=true;duplicate.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;duplicate.payload.size=1;duplicate.payload.bytes[0]='x';
  uint8_t duplicateData[239]{};auto duplicateOut=pb_ostream_from_buffer(duplicateData,sizeof(duplicateData));assert(pb_encode(&duplicateOut,meshtastic_Data_fields,&duplicate));
  assert(service.codec.encodeZeroHopData(peer,UINT32_MAX,2,duplicateData,duplicateOut.bytes_written,same,sizeof(same),sameSize));
  const auto beforeDuplicate=hot.calls;
  service.rx(same,sameSize,0,10,123,true,nullptr,false,1792660000u,10);
  assert(hot.calls==beforeDuplicate+1&&!hot.viaMqtt&&hot.utc==1792660000u);
  same[12]|=0x10u; // A repeat can carry newer stock origin metadata without changing packet identity.
  service.rx(same,sameSize,0,11,124,true,nullptr,false,1792660001u,11);
  assert(hot.calls==beforeDuplicate+2&&hot.viaMqtt&&hot.utc==1792660001u&&hot.uptime==11);

  Radio mutedRadio;mt::Service muted(mutedRadio,false,self,127,true,false);mt::test::KnownChannelFixtureClock mutedClock;mutedClock.bind(muted);HotObservation mutedHot;
  assert(muted.bindHotNodeObservationSink(HotObservation::receive,&mutedHot));auto mutedConfig=muted.config;assert(mutedConfig.setDeviceRole(meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,0));
  mutedConfig.bytes[4]=1;auto mutedNode=mutedConfig.node();mutedNode.setPublic();mutedConfig.node(mutedNode);assert(muted.apply(mutedConfig)&&muted.start(false,0));
  assert(muted.receiving()&&!muted.relaying()&&!muted.endpointArmed()&&!muted.txAllowed());
  auto mutedData=pb_ostream_from_buffer(duplicateData,sizeof(duplicateData));assert(pb_encode(&mutedData,meshtastic_Data_fields,&duplicate));
  assert(muted.codec.encodeZeroHopData(peer,UINT32_MAX,44,duplicateData,mutedData.bytes_written,same,sizeof(same),sameSize));
  muted.rx(same,sameSize,0,10,123,true,nullptr,false,0,10);assert(mutedHot.calls==1&&mutedHot.uptime==10);
  // Real extension editor storage pause, verified readback, no old-key writes.
  Memory memory;mt::modules::ExtendedModuleJournal journal;assert(!journal.load(memory));mt::modules::ExtendedModuleEdit edit(service,journal,memory);assert(edit.load());
  meshtastic_ModuleConfig module{};module.which_payload_variant=meshtastic_ModuleConfig_traffic_management_tag;module.payload_variant.traffic_management=cfg;
  // The implemented direct-response option participates in the same staged, verified journal.
  module.payload_variant.traffic_management.nodeinfo_direct_response_max_hops=1;
  assert(!edit.save(module,20,true)&&edit.dirty()&&!memory.writes);assert(!edit.commit(21)&&!edit.dirty()&&edit.trusted()&&memory.writes==1&&service.receiving());
  meshtastic_ModuleConfig_TrafficManagementConfig saved{};journal.config().traffic().get(saved);
  assert(saved.nodeinfo_direct_response_max_hops==1&&saved.rate_limit_max_packets==cfg.rate_limit_max_packets);
  assert(memory.files.size()==1&&memory.files.begin()->first.rfind("mtextra/",0)==0);
  radio.occupied=true;const auto before=memory.files;module.payload_variant.traffic_management.rate_limit_max_packets=3;
  assert(edit.save(module,22,false)==-EBUSY&&memory.files==before&&edit.trusted());radio.occupied=false;
  memory.fail=true;assert(edit.save(module,23,false)==-EIO&&!edit.ready()&&!service.receiving()&&!radio.sends);
}
