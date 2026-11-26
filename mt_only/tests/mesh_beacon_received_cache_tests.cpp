// SPDX-License-Identifier: GPL-3.0-only
#include "modules/MeshBeaconReceivedCache.h"
#include "modules/ExtendedModuleEdit.h"
#include "NodeAdmin.h"
#include "Service.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <vector>

using mt::beacon::MeshBeaconReceivedCache;
namespace mt {
struct ReceivedBeaconCacheTestAccess {
  static bool copy(const mt::Service &s,uint32_t source,uint8_t channel,const uint8_t *key,size_t keyLength,
                   uint64_t now,uint8_t *out,size_t capacity,size_t *length){
    return s.receivedBeacons_.copy(source,channel,key,keyLength,now,out,capacity,length);
  }
};
}
using mt::ReceivedBeaconCacheTestAccess;
static size_t payload(uint8_t *out,size_t cap,bool withText=true){
  meshtastic_MeshBeacon b=meshtastic_MeshBeacon_init_zero;
  b.has_offer_channel=true;b.offer_channel=meshtastic_ChannelSettings_init_zero;
  b.offer_channel.psk.size=16;memset(b.offer_channel.psk.bytes,0xA7,16);
  if(withText)strcpy(b.message,"advisory");
  auto s=pb_ostream_from_buffer(out,cap);assert(pb_encode(&s,meshtastic_MeshBeacon_fields,&b));return s.bytes_written;
}
struct Radio:mt::Radio {
  bool on=false;bool ready()override{return true;}int start()override{on=true;return 0;}
  int restore()override{on=true;return 0;}int stop()override{on=false;return 0;}
  int health()override{return on?1:0;}bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{return 0;}
  bool completion(int&)override{return false;}
};
struct Backend:mt::SettingsBackend {
  std::vector<uint8_t> bytes;
  int read(const char*,uint8_t *out,size_t cap)override{if(bytes.empty())return -ENOENT;if(bytes.size()>cap)return -E2BIG;memcpy(out,bytes.data(),bytes.size());return int(bytes.size());}
  int write(const char*,const uint8_t *in,size_t n)override{bytes.assign(in,in+n);return 0;}
};
static bool sendBeacon(mt::Service &service,uint32_t source,uint32_t id,uint64_t uptime,const uint8_t *beacon,size_t length,
                       uint32_t bitfield=0,uint32_t requestId=0,bool unknownField=false){
  meshtastic_Data d=meshtastic_Data_init_zero;d.has_bitfield=true;d.portnum=meshtastic_PortNum_MESH_BEACON_APP;
  d.bitfield=bitfield;d.request_id=requestId;
  memcpy(d.payload.bytes,beacon,length);d.payload.size=pb_size_t(length);
  uint8_t data[239]{},frame[255]{};auto stream=pb_ostream_from_buffer(data,sizeof data);
  if(!pb_encode(&stream,meshtastic_Data_fields,&d))return false;
  if(unknownField){if(stream.bytes_written+2>sizeof data)return false;data[stream.bytes_written++]=0x58;data[stream.bytes_written++]=0x01;}
  size_t frameLength=0;
  if(!service.codec.encodeZeroHopData(source,UINT32_MAX,id,data,stream.bytes_written,frame,sizeof frame,frameLength))return false;
  service.trafficManagement(nullptr,uptime);service.rx(frame,frameLength,0,uint32_t(uptime),17);return true;
}
int main(){
  MeshBeaconReceivedCache c;uint8_t p[233]{},out[233]{};const size_t n=payload(p,sizeof p,false);
  uint8_t k1[16]{},k2[16]{};k1[0]=1;k2[0]=2;size_t len=0;
  static_assert(sizeof(MeshBeaconReceivedCache)<=576,"bounded cache");
  assert(c.remember(7,0,k1,sizeof k1,p,n,100,100));
  assert(c.remember(7,1,k1,sizeof k1,p,n,100,100));
  assert(!c.copy(7,0,k2,sizeof k2,100,out,sizeof out,&len));
  assert(c.copy(7,0,k1,sizeof k1,100,out,sizeof out,&len)&&len==n&&!memcmp(out,p,n));
  assert(!c.remember(8,0,k1,sizeof k1,p,n,100,100)); // two live slots, no eviction
  assert(c.copy(7,0,k1,sizeof k1,100+MeshBeaconReceivedCache::TtlMs,out,sizeof out,&len));
  assert(!c.copy(7,0,k1,sizeof k1,101+MeshBeaconReceivedCache::TtlMs,out,sizeof out,&len));
  assert(c.remember(9,0,k1,sizeof k1,p,n,101+MeshBeaconReceivedCache::TtlMs,101+MeshBeaconReceivedCache::TtlMs));
  assert(c.forget(7,1,k1,sizeof k1));assert(!c.copy(7,1,k1,sizeof k1,200,out,sizeof out,&len));
  c.clear();assert(!c.copy(7,0,k1,sizeof k1,200,out,sizeof out,&len));
  const uint64_t beyond32=0x100000123ULL;
  assert(c.remember(10,0,k1,sizeof k1,p,n,beyond32,beyond32));
  assert(c.copy(10,0,k1,sizeof k1,beyond32+MeshBeaconReceivedCache::TtlMs,out,sizeof out,&len));
  assert(!c.copy(10,0,k1,sizeof k1,beyond32+MeshBeaconReceivedCache::TtlMs+1,out,sizeof out,&len));
  c.clear();
  assert(!c.remember(7,0,k1,2,p,n,1,1));
  p[n]=0x80;assert(!c.remember(7,0,k1,sizeof k1,p,n+1,1,1));
  // Exercise the real Service path: listener disabled by default, canonical
  // port37 accepted only after normal decode, then disabling clears immediately.
  uint8_t valid[233]{};const size_t validLength=payload(valid,sizeof valid);Radio radio;
  constexpr uint32_t self=0x12345678,source=0x24680135;
  mt::Service service(radio,true,self,127,true,false);mt::Config config;config.bytes[4]=1;
  auto record=config.node();record.setPublic();config.node(record);
  assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
  assert(sendBeacon(service,source,71,1000,valid,validLength));
  auto selection=mt::PrimaryChannel::Selection{};assert(service.config.channelSelection(0,selection));
  assert(!ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,1000,out,sizeof out,&len));
  service.meshBeaconListening(true);
  assert(sendBeacon(service,source,72,2000,valid,validLength));
  assert(ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,2000,out,sizeof out,&len));
  meshtastic_MeshBeacon cached{};auto cachedStream=pb_istream_from_buffer(out,len);
  assert(pb_decode(&cachedStream,meshtastic_MeshBeacon_fields,&cached));
  assert(cached.message[0]=='\0'&&cached.has_offer_channel&&cached.offer_channel.psk.size==16);
  // Service cache admission uses the same complete canonical Data gate as
  // phone forwarding. Each of these malformed outer envelopes must be inert.
  for(unsigned malformed=0;malformed<3;++malformed){
    const uint32_t badSource=source+malformed+1,id=80+malformed;const uint64_t now=2100+malformed;
    assert(sendBeacon(service,badSource,id,now,valid,validLength,malformed==0?2:0,malformed==1?99:0,malformed==2));
    assert(!ReceivedBeaconCacheTestAccess::copy(service,badSource,0,selection.bytes,selection.length,now,out,sizeof out,&len));
  }
  assert(sendBeacon(service,source,73,3000,valid,validLength));
  mt::modules::ExtendedModuleJournal journal;Backend backend;assert(!journal.load(backend));
  mt::modules::ExtendedModuleEdit edit(service,journal,backend);assert(edit.load());
  assert(ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,3000,out,sizeof out,&len));
  assert(sendBeacon(service,source,74,4000,valid,validLength));
  assert(ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,4000,out,sizeof out,&len));
  meshtastic_ModuleConfig module{};module.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
  assert(edit.config().beacon(module.payload_variant.mesh_beacon));
  module.payload_variant.mesh_beacon.flags &= ~uint32_t(meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED);
  assert(edit.save(module,5000,false)==0);
  assert(!ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,4000,out,sizeof out,&len));
  service.meshBeaconListening(true);assert(sendBeacon(service,source,75,6000,valid,validLength));
  service.stop();auto changed=service.config;assert(changed.setHopLimit(4)&&service.apply(changed));
  assert(!ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,6000,out,sizeof out,&len));
  Backend nodeBackend;mt::NodeJournal nodeJournal;assert(nodeJournal.load(nodeBackend,service.nodes,self)==0);
  service.meshBeaconListening(true);assert(service.start(false,7000));
  const uint64_t serviceBeyond32=0x100001f40ULL;
  assert(sendBeacon(service,source,76,serviceBeyond32,valid,validLength));
  assert(ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,serviceBeyond32,out,sizeof out,&len));
  assert(ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,serviceBeyond32+MeshBeaconReceivedCache::TtlMs,out,sizeof out,&len));
  assert(!ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,serviceBeyond32+MeshBeaconReceivedCache::TtlMs+1,out,sizeof out,&len));
  assert(service.armEndpoint(false));
  assert(mt::NodeAdmin::edit(service,radio,nodeJournal,nodeBackend,source,mt::NodeCache::Edit::Remove,7001)==0);
  assert(!ReceivedBeaconCacheTestAccess::copy(service,source,0,selection.bytes,selection.length,serviceBeyond32,out,sizeof out,&len));
  mt::PrimaryChannel::clear(selection);
  printf("PASS cache_size=%zu entry_size=%zu capacity=%zu ttl_ms=%llu\n",sizeof c,sizeof(MeshBeaconReceivedCache::Entry),MeshBeaconReceivedCache::Capacity,(unsigned long long)MeshBeaconReceivedCache::TtlMs);
}
