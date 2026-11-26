// SPDX-License-Identifier: GPL-3.0-only
#include "modules/ExtendedModuleJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using Settings=mt::modules::ExtendedModuleSettings;
using Journal=mt::modules::ExtendedModuleJournal;
struct Backend:mt::SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> records;
 std::vector<std::string> reads,writes;
 bool torn=false,ambiguous=false,badReadback=false;
 int read(const char *key,uint8_t*out,size_t capacity)override{
  reads.emplace_back(key);auto i=records.find(key);if(i==records.end())return -ENOENT;
  if(i->second.size()>capacity)return -E2BIG;
  memcpy(out,i->second.data(),i->second.size());
  if(badReadback&&!writes.empty()&&writes.back()==key&&i->second.size())out[0]^=1;
  return int(i->second.size());
 }
 int write(const char *key,const uint8_t*data,size_t n)override{
  writes.emplace_back(key);records[key]={data,data+n};
  if(torn)records[key].resize(n/2);
  return ambiguous?-EIO:0;
 }
};
static meshtastic_ModuleConfig traffic(uint32_t value){
 meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_traffic_management_tag;
 auto &t=m.payload_variant.traffic_management;t.position_min_interval_secs=value;t.nodeinfo_direct_response_max_hops=value;
 t.rate_limit_window_secs=value;t.rate_limit_max_packets=value;t.unknown_packet_threshold=value;return m;
}
static meshtastic_ModuleConfig beacon(){
 meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;return m;
}
static void rawEnum(void *out,size_t size,uint32_t value){memcpy(out,&value,size);}
static uint32_t outerCrc(const uint8_t *p,size_t n){
 uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;
}
static void put32le(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
static void put16le(uint8_t *p,uint16_t value){p[0]=uint8_t(value);p[1]=uint8_t(value>>8);}
static void sealInner(uint8_t *p){auto c=mt::Config::checksum(p,318);p[318]=uint8_t(c);p[319]=uint8_t(c>>8);}
static void sealOuter(std::vector<uint8_t> &record){put32le(record.data()+332,outerCrc(record.data(),332));}
static void sealSparse(std::vector<uint8_t> &record){
 assert(record.size()>=Journal::V2FixedSize+4);put32le(record.data()+record.size()-4,outerCrc(record.data(),record.size()-4));
}
static std::vector<uint8_t> denseV1(const Settings &settings,uint32_t generation){
 std::vector<uint8_t> record(Journal::RecordSize);record[0]='Z';record[1]='T';record[2]='X';record[3]=Journal::DenseVersion;
 put32le(record.data()+4,generation);put16le(record.data()+8,Settings::Size);
 memcpy(record.data()+Journal::HeaderSize,settings.bytes,Settings::Size);sealOuter(record);return record;
}
static std::vector<uint8_t> sparseV2(const Settings &settings,uint32_t generation){
 std::vector<uint8_t> record(Journal::RecordSize);record[0]='Z';record[1]='T';record[2]='X';record[3]=Journal::SparseVersion;
 put32le(record.data()+4,generation);put16le(record.data()+8,Settings::Size);size_t packed=0;
 for(uint8_t value:settings.bytes)if(value)++packed;put16le(record.data()+10,uint16_t(packed));size_t at=Journal::V2FixedSize;
 for(size_t i=0;i<Settings::Size;++i)if(settings.bytes[i]){record[Journal::HeaderSize+i/8]|=uint8_t(1u<<(i%8));record[at++]=settings.bytes[i];}
 record.resize(at+4);sealSparse(record);return record;
}
static std::vector<uint8_t> legacyV1(const Settings &settings,uint32_t generation=1){
 auto record=denseV1(settings,generation);uint8_t *inner=record.data()+Journal::HeaderSize;
 inner[3]=Settings::LegacyVersion;memset(inner+273,0,45);sealInner(inner);sealOuter(record);return record;
}
static void rejects(const std::vector<uint8_t> &record){
 Backend backend;backend.records["mtextra/state0"]=record;Journal journal;
 assert(journal.load(backend)==-EINVAL&&journal.state()==Journal::State::Invalid);
}
int main(){
 Settings s;assert(s.valid()&&!s.sentBefore()&&!s.lastSentUtc());
 meshtastic_ModuleConfig out{};assert(s.get(out,meshtastic_ModuleConfig_traffic_management_tag));
 assert(out.payload_variant.traffic_management.position_min_interval_secs==21600);
 assert(out.payload_variant.traffic_management.rate_limit_max_packets==0);
 assert(s.get(out,meshtastic_ModuleConfig_mesh_beacon_tag));assert(out.payload_variant.mesh_beacon.flags==5);
 assert(!out.payload_variant.mesh_beacon.broadcast_interval_secs);
 auto cannedConfig=meshtastic_ModuleConfig_CannedMessageConfig{};cannedConfig.enabled=true;cannedConfig.send_bell=true;
 memcpy(cannedConfig.allow_input_source,"_any",5);assert(s.setCanned(cannedConfig)&&s.cannedEnabled()&&s.cannedBell());
 meshtastic_ModuleConfig_CannedMessageConfig cannedRoundtrip{};
 assert(s.canned(cannedRoundtrip)&&cannedRoundtrip.enabled&&cannedRoundtrip.send_bell&&
        !strcmp(cannedRoundtrip.allow_input_source,"_any"));
 auto invalidCanned=cannedConfig;invalidCanned.inputbroker_pin_a=1;const auto beforeInvalid=s;
 assert(!s.setCanned(invalidCanned)&&s.same(beforeInvalid));
 invalidCanned=cannedConfig;memcpy(invalidCanned.allow_input_source,"rotEnc1",8);
 assert(!s.setCanned(invalidCanned)&&s.same(beforeInvalid));
 auto serialConfig=meshtastic_ModuleConfig_SerialConfig{};serialConfig.enabled=true;serialConfig.echo=true;
 serialConfig.rxd=UINT32_MAX;serialConfig.txd=0x80000000u;
 serialConfig.baud=meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_921600;
 serialConfig.timeout=UINT32_MAX;serialConfig.mode=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG;
 assert(s.setSerial(serialConfig));meshtastic_ModuleConfig_SerialConfig serialRoundtrip{};
 assert(s.serial(serialRoundtrip)&&serialRoundtrip.enabled&&serialRoundtrip.echo&&serialRoundtrip.rxd==UINT32_MAX&&
        serialRoundtrip.txd==0x80000000u&&uint32_t(serialRoundtrip.baud)==uint32_t(serialConfig.baud)&&
        serialRoundtrip.timeout==UINT32_MAX&&uint32_t(serialRoundtrip.mode)==uint32_t(serialConfig.mode));
 meshtastic_ModuleConfig serialModule{};serialModule.which_payload_variant=meshtastic_ModuleConfig_serial_tag;
 serialModule.payload_variant.serial=serialConfig;assert(s.set(serialModule)&&s.get(out,meshtastic_ModuleConfig_serial_tag));
 auto invalidSerial=serialConfig;rawEnum(&invalidSerial.mode,sizeof(invalidSerial.mode),UINT32_MAX);const auto beforeSerial=s;
 assert(!s.setSerial(invalidSerial)&&s.same(beforeSerial));
 invalidSerial=serialConfig;rawEnum(&invalidSerial.baud,sizeof(invalidSerial.baud),UINT32_MAX);
 assert(!s.setSerial(invalidSerial)&&s.same(beforeSerial));
 invalidSerial=serialConfig;invalidSerial.override_console_serial_port=true;
 assert(!s.setSerial(invalidSerial)&&s.same(beforeSerial));
 auto invalidCannedEnum=cannedConfig;rawEnum(&invalidCannedEnum.inputbroker_event_press,sizeof(invalidCannedEnum.inputbroker_event_press),UINT32_MAX);
 assert(!s.setCanned(invalidCannedEnum)&&s.same(beforeSerial));
 auto empty=beacon();assert(s.set(empty)&&s.valid());assert(s.get(out,empty.which_payload_variant)&&!out.payload_variant.mesh_beacon.flags);
 for(auto v:{uint32_t(0),uint32_t(1),UINT32_MAX}){
  auto t=traffic(v);assert(s.set(t)&&s.get(out,t.which_payload_variant));const auto &r=out.payload_variant.traffic_management;
  assert(r.position_min_interval_secs==v&&r.nodeinfo_direct_response_max_hops==v&&r.rate_limit_window_secs==v&&r.rate_limit_max_packets==v&&r.unknown_packet_threshold==v);
 }
 s.markSent(0);assert(s.sentBefore()&&!s.lastSentUtc());s.markSent(1700000000);assert(s.valid()&&s.lastSentUtc()==1700000000);
 auto b=beacon();auto &v=b.payload_variant.mesh_beacon;v.flags=7;v.broadcast_interval_secs=UINT32_MAX;
 memset(v.broadcast_message,'x',sizeof(v.broadcast_message)-1);v.broadcast_message[sizeof(v.broadcast_message)-1]=0;
 v.has_broadcast_offer_channel=true;v.broadcast_offer_channel.psk.size=32;memset(v.broadcast_offer_channel.psk.bytes,0xa5,32);
 memcpy(v.broadcast_offer_channel.name,"test",5);v.broadcast_offer_channel.has_module_settings=true;v.broadcast_offer_channel.module_settings.position_precision=32;
 v.broadcast_offer_region=meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM;v.has_broadcast_offer_preset=true;v.broadcast_offer_preset=meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO;
 assert(s.set(b)&&s.valid()&&s.get(out,b.which_payload_variant));assert(out.payload_variant.mesh_beacon.broadcast_interval_secs==UINT32_MAX&&strlen(out.payload_variant.mesh_beacon.broadcast_message)==60);
 assert(s.sentBefore()&&s.lastSentUtc()==1700000000);const auto original=s;
 auto bad=b;rawEnum(&bad.payload_variant.mesh_beacon.broadcast_offer_region,sizeof(bad.payload_variant.mesh_beacon.broadcast_offer_region),255);assert(!s.set(bad)&&s.same(original));
 bad=b;rawEnum(&bad.payload_variant.mesh_beacon.broadcast_offer_preset,sizeof(bad.payload_variant.mesh_beacon.broadcast_offer_preset),255);assert(!s.set(bad)&&s.same(original));
 bad=b;bad.payload_variant.mesh_beacon.flags=8;assert(!s.set(bad)&&s.same(original));
 auto withTarget=b;withTarget.payload_variant.mesh_beacon.broadcast_targets_count=1;
 auto targetSettings=s;assert(targetSettings.set(withTarget)&&targetSettings.valid());
 bad=b;bad.payload_variant.mesh_beacon.broadcast_targets_count=5;assert(!s.set(bad)&&s.same(original));
 bad=b;bad.payload_variant.mesh_beacon.broadcast_offer_channel.psk.size=31;assert(!s.set(bad)&&s.same(original));
 bad=b;memset(bad.payload_variant.mesh_beacon.broadcast_message,'x',sizeof(bad.payload_variant.mesh_beacon.broadcast_message));assert(!s.set(bad)&&s.same(original));
 for(size_t i=0;i<Settings::Size;++i){auto c=s;c.bytes[i]^=1;assert(!c.valid());}
 auto reserved=s;reserved.bytes[Settings::TakReservedStart]=1;reserved.seal();assert(!reserved.valid());
 auto over=s;over.bytes[24]=243;over.bytes[25]=0;over.seal();assert(!over.valid());
 Backend backend;backend.records["mtbase/state0"]={1,2,3};backend.records["mtmod/state0"]={4,5,6};
 const auto preserved=backend.records;Journal j;assert(j.load(backend)==0&&j.state()==Journal::State::Fresh&&backend.writes.empty());
 assert(j.save(backend,s)==0&&j.state()==Journal::State::Loaded);const auto first=backend.records.at("mtextra/state0");
 assert(first.size()<Journal::RecordSize&&first[0]=='Z'&&first[1]=='T'&&first[2]=='X'&&first[3]==Journal::SparseVersion&&first[8]==0x40&&first[9]==1);
 const size_t packed=size_t(first[10])|(size_t(first[11])<<8);
 assert(first.size()==Journal::V2FixedSize+packed+4&&first.size()<Journal::RecordSize);
 std::vector<uint8_t> unpacked(Settings::Size);size_t packedAt=Journal::V2FixedSize;
 for(size_t i=0;i<Settings::Size;++i)if(first[Journal::HeaderSize+i/8]&(1u<<(i%8)))unpacked[i]=first[packedAt++];
 assert(packedAt==Journal::V2FixedSize+packed&&!memcmp(unpacked.data(),s.bytes,Settings::Size));
 assert(unpacked[3]==Settings::Version&&unpacked[26]!=0&&unpacked[318]!=0&&unpacked[319]!=0);
 const auto writeCount=backend.writes.size();assert(j.save(backend,s)==0&&backend.writes.size()==writeCount);
 Journal compactReload;assert(compactReload.load(backend)==0&&compactReload.state()==Journal::State::Loaded&&
  compactReload.generation()==1&&compactReload.config().same(s));
 // Compact v2 is canonical: CRC, length, bitmap/value correspondence, and
 // exact inner settings CRC are all checked independently of the outer CRC.
 auto badSparse=first;badSparse[Journal::V2FixedSize]^=1;rejects(badSparse);
 badSparse=first;put16le(badSparse.data()+10,uint16_t(packed+1));sealSparse(badSparse);rejects(badSparse);
 badSparse=first;badSparse.insert(badSparse.end()-4,0x55);sealSparse(badSparse);rejects(badSparse);
 badSparse=first;badSparse.pop_back();rejects(badSparse);
 badSparse=first;const size_t zeroIndex=Settings::TakReservedStart;assert(s.bytes[zeroIndex]==0);
 size_t beforeZero=0;for(size_t i=0;i<zeroIndex;++i)if(first[Journal::HeaderSize+i/8]&(1u<<(i%8)))++beforeZero;
 badSparse.insert(badSparse.begin()+Journal::V2FixedSize+beforeZero,0);
 badSparse[Journal::HeaderSize+zeroIndex/8]|=uint8_t(1u<<(zeroIndex%8));
 put16le(badSparse.data()+10,uint16_t(packed+1));sealSparse(badSparse);rejects(badSparse);
 badSparse=first;size_t dataByte=0;for(size_t i=0;i<4;++i)if(first[Journal::HeaderSize+i/8]&(1u<<(i%8)))++dataByte;
 badSparse[Journal::V2FixedSize+dataByte]^=0x80;sealSparse(badSparse);rejects(badSparse);
 // A 2.8.1 saved record can contain 100 bytes. The 60th byte here
 // splits a two-byte character; keep the preceding 59 bytes and all preferences.
 Settings sanitize=s;auto pins=beacon();auto &wire=pins.payload_variant.mesh_beacon;
 wire.broadcast_interval_secs=2;wire.has_broadcast_offer_frequency_slot=true;wire.broadcast_offer_frequency_slot=0;
 wire.broadcast_targets_count=2;
 for(auto &t:wire.broadcast_targets){t.has_preset=true;t.has_frequency_slot=true;t.frequency_slot=9999;}
 wire.broadcast_targets[0].region=meshtastic_Config_LoRaConfig_RegionCode_US;
 assert(sanitize.set(pins));meshtastic_ModuleConfig_MeshBeaconConfig normalized{};
 assert(sanitize.beacon(normalized)&&normalized.broadcast_interval_secs==3600&&!normalized.has_broadcast_offer_frequency_slot);
 assert(!normalized.broadcast_targets[0].has_frequency_slot&&normalized.broadcast_targets[1].has_frequency_slot);
 Settings longText=s;
 memset(longText.bytes+26,0,Settings::BeaconCapacity);
 longText.bytes[24]=102;longText.bytes[25]=0;
 longText.bytes[26]=0x22;longText.bytes[27]=100;
 memset(longText.bytes+28,'x',100);longText.bytes[28+59]=0xc3;longText.bytes[28+60]=0xa9;
 longText.seal();Backend old;old.records["mtextra/state0"]=denseV1(longText,19);Journal adapted;
 assert(adapted.load(old)==0 && adapted.generation()==19 && old.writes.empty());
 meshtastic_ModuleConfig_MeshBeaconConfig shortened{};
 assert(adapted.config().beacon(shortened)&&strlen(shortened.broadcast_message)==59);
 assert(!memcmp(adapted.config().bytes+268,s.bytes+268,50));
 auto malformed=longText;malformed.bytes[27]=101;malformed.seal();rejects(denseV1(malformed,19));
 auto corrupt=longText;corrupt.bytes[318]^=1;rejects(denseV1(corrupt,19));
 Backend denseLegacy;denseLegacy.records["mtextra/state0"]=denseV1(s,7);Journal denseReload;
 assert(denseReload.load(denseLegacy)==0&&denseReload.config().same(s)&&denseReload.generation()==7&&denseLegacy.writes.empty());
 // v1 must be authenticated as its original zero-reserved layout before
 // module fields are defaulted and the inner CRC is resealed as v2.
 Backend legacy;legacy.records["mtextra/state0"]=legacyV1(s,9);
 Journal migrated;assert(migrated.load(legacy)==0&&migrated.state()==Journal::State::Loaded);
 assert(migrated.config().valid()&&migrated.config().bytes[3]==Settings::Version&&migrated.generation()==9&&legacy.writes.empty());
 const auto migratedValue=migrated.config();assert(migrated.save(legacy,migratedValue)==0&&legacy.writes.empty());
 meshtastic_ModuleConfig_CannedMessageConfig cannedOut{};
 assert(migrated.config().canned(cannedOut)&&!cannedOut.enabled&&!cannedOut.send_bell);
 meshtastic_ModuleConfig_SerialConfig serialOut{};
 assert(migrated.config().serial(serialOut)&&!serialOut.enabled&&!serialOut.echo&&!serialOut.rxd&&!serialOut.txd&&!serialOut.timeout);
 auto badLegacy=legacy.records["mtextra/state0"];badLegacy[16]^=1;sealOuter(badLegacy);
 Backend corruptV1;corruptV1.records["mtextra/state0"]=badLegacy;Journal rejectsCrc;
 assert(rejectsCrc.load(corruptV1)==-EINVAL&&rejectsCrc.state()==Journal::State::Invalid);
 badLegacy=legacy.records["mtextra/state0"];badLegacy[12+273]=1;sealInner(badLegacy.data()+12);sealOuter(badLegacy);
 Backend reservedV1;reservedV1.records["mtextra/state0"]=badLegacy;Journal rejectsReserved;
 assert(rejectsReserved.load(reservedV1)==-EINVAL&&rejectsReserved.state()==Journal::State::Invalid);
 // Generation ordering remains wrap-safe across dense legacy and sparse v2.
 auto wrappedNext=s;assert(wrappedNext.set(traffic(0)));
 Backend wrap;wrap.records["mtextra/state0"]=denseV1(s,UINT32_MAX);
 wrap.records["mtextra/state1"]=sparseV2(wrappedNext,0);Journal wrapLoad;
 assert(wrapLoad.load(wrap)==0&&wrapLoad.state()==Journal::State::Loaded&&wrapLoad.activeSlot()==1&&
        wrapLoad.generation()==0&&wrapLoad.config().same(wrappedNext));
 assert(backend.records.at("mtbase/state0")==preserved.at("mtbase/state0")&&backend.records.at("mtmod/state0")==preserved.at("mtmod/state0"));
 for(auto &key:backend.reads)assert(key=="mtextra/state0"||key=="mtextra/state1");for(auto &key:backend.writes)assert(key=="mtextra/state0"||key=="mtextra/state1");
 auto next=s;assert(next.set(traffic(0)));backend.ambiguous=true;assert(j.save(backend,next)==-EIO&&j.state()==Journal::State::IoError&&j.config().same(s));
 const auto afterFailure=backend.writes.size();assert(j.save(backend,s)==-EACCES&&backend.writes.size()==afterFailure);backend.ambiguous=false;
 Journal reload;assert(reload.load(backend)==0&&reload.config().same(next));
 backend.records["mtextra/state0"].resize(4);Journal recovered;assert(recovered.load(backend)==0&&recovered.state()==Journal::State::Recovered);
 assert(recovered.save(backend,next)==0&&recovered.state()==Journal::State::Loaded&&backend.writes.size()==afterFailure+1);
 backend.badReadback=true;assert(recovered.save(backend,s)==-EIO&&recovered.state()==Journal::State::IoError);backend.badReadback=false;
 Backend torn;Journal t;assert(t.load(torn)==0);torn.torn=true;assert(t.save(torn,s)==-EIO&&t.state()==Journal::State::IoError);
 // Maximize the public supported fields to exercise the dense fallback when
 // this valid inner schema can reach its strict V1/V2 size boundary.
 Settings fullest;auto fullTraffic=traffic(UINT32_MAX);assert(fullest.set(fullTraffic));
 meshtastic_ModuleConfig_CannedMessageConfig fullCanned{};fullCanned.enabled=true;fullCanned.send_bell=true;
 memcpy(fullCanned.allow_input_source,"_any",5);assert(fullest.setCanned(fullCanned));
 auto fullSerial=serialConfig;fullSerial.enabled=true;fullSerial.echo=true;fullSerial.rxd=UINT32_MAX;
 fullSerial.txd=UINT32_MAX;fullSerial.timeout=UINT32_MAX;assert(fullest.setSerial(fullSerial));
 meshtastic_ModuleConfig fullBeacon{};fullBeacon.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
 auto &fb=fullBeacon.payload_variant.mesh_beacon;fb.flags=7;fb.broadcast_interval_secs=UINT32_MAX;
 memset(fb.broadcast_message,'z',sizeof(fb.broadcast_message)-1);fb.broadcast_message[sizeof(fb.broadcast_message)-1]=0;fb.has_broadcast_offer_channel=true;
 memcpy(fb.broadcast_offer_channel.name,"12345678901",12);fb.broadcast_offer_channel.psk.size=32;
 fb.broadcast_offer_channel.channel_num=UINT32_MAX;fb.broadcast_offer_channel.id=UINT32_MAX;
 fb.broadcast_offer_channel.uplink_enabled=true;fb.broadcast_offer_channel.downlink_enabled=true;
 fb.broadcast_offer_channel.use_aead=true;
 memset(fb.broadcast_offer_channel.psk.bytes,0xff,32);fb.broadcast_offer_channel.has_module_settings=true;
 fb.broadcast_offer_channel.module_settings.position_precision=32;fb.broadcast_offer_channel.module_settings.is_muted=true;
 fb.broadcast_offer_region=meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM;
 fb.has_broadcast_offer_preset=true;fb.broadcast_offer_preset=meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO;
 assert(fullest.set(fullBeacon)&&fullest.valid());fullest.markSent(UINT32_MAX);
 size_t fullNonzero=0;for(uint8_t value:fullest.bytes)if(value)++fullNonzero;
 Backend thresholdBackend;Journal thresholdJournal;assert(thresholdJournal.load(thresholdBackend)==0);
 assert(thresholdJournal.save(thresholdBackend,fullest)==0);
 const auto &thresholdRecord=thresholdBackend.records.at("mtextra/state0");
 if(Journal::V2FixedSize+fullNonzero+4<Journal::RecordSize)
  assert(thresholdRecord.size()<Journal::RecordSize&&thresholdRecord[3]==Journal::SparseVersion);
 else assert(thresholdRecord.size()==Journal::RecordSize&&thresholdRecord[3]==Journal::DenseVersion);
 printf("compact threshold fixture: valid_nonzero=%zu dense_fallback=%u stored_bytes=%zu version=%u\n",
        fullNonzero,unsigned(Journal::V2FixedSize+fullNonzero+4>=Journal::RecordSize),
        thresholdRecord.size(),unsigned(thresholdRecord[3]));
 puts("PASS extended settings journal: sparse v2 exact roundtrip/canonical rejection, dense v1 compatibility and inner-v1 migration, wrap-safe generations, canned/serial bounds, independence, recovery and uncertain writes");
}
