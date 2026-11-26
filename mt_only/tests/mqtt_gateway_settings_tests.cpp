// SPDX-License-Identifier: GPL-3.0-only
#include "network/MqttGatewayConfigMapping.h"
#include "network/MqttGatewayJournal.h"
#include "network/MqttGatewayAdmin.h"
#include "network/MqttBootSnapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pb_decode.h"
#include "pb_encode.h"

namespace {
using mt::network::MqttGatewayConfigMapping;
using mt::network::MqttGatewayJournal;
using mt::network::MqttGatewaySettings;
constexpr size_t Rows=8, RecordSize=MqttGatewayJournal::RecordSize;
struct Row {char key[20]{};uint8_t bytes[RecordSize+8]{};size_t size=0;};
struct Backend final:mt::SettingsBackend {
  Row rows[Rows]{};char readKeys[32][20]{};char writeKeys[16][20]{};size_t reads=0,writes=0;
  int writeError=0;bool tearWrite=false,alterReadback=false;
  Row *find(const char *key){for(auto &r:rows)if(r.key[0]&&!strcmp(r.key,key))return &r;return nullptr;}
  int read(const char *key,uint8_t *out,size_t cap)override{
    assert(reads<32);strncpy(readKeys[reads++],key,19);Row *r=find(key);if(!r)return -ENOENT;
    if(r->size>cap)return -E2BIG;memcpy(out,r->bytes,r->size);return int(r->size);
  }
  int write(const char *key,const uint8_t *data,size_t n)override{
    assert(writes<16);strncpy(writeKeys[writes++],key,19);if(writeError)return writeError;
    Row *r=find(key);if(!r)for(auto &candidate:rows)if(!candidate.key[0]){r=&candidate;strncpy(r->key,key,sizeof(r->key)-1);break;}
    assert(r);r->size=tearWrite?n/2:n;memcpy(r->bytes,data,r->size);
    if(alterReadback&&r->size>16)r->bytes[16]^=1;return 0;
  }
  bool touchedOnlyMqtt()const{
    for(size_t i=0;i<reads;++i)if(strcmp(readKeys[i],"mtmqtt/state0")&&strcmp(readKeys[i],"mtmqtt/state1"))return false;
    for(size_t i=0;i<writes;++i)if(strcmp(writeKeys[i],"mtmqtt/state0")&&strcmp(writeKeys[i],"mtmqtt/state1"))return false;
    return true;
  }
};
struct TestRadio final:mt::Radio {
  int state=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 1;}
  int send(uint8_t *,size_t)override{return 0;}
  bool completion(int &)override{return false;}
};
uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
meshtastic_ModuleConfig_MQTTConfig storedConfig(){
  meshtastic_ModuleConfig_MQTTConfig c=meshtastic_ModuleConfig_MQTTConfig_init_zero;
  strcpy(c.address,"broker.native.invalid");strcpy(c.username,"fixture-user");strcpy(c.password,"synthetic-only");strcpy(c.root,"lab/test");
  c.encryption_enabled=true;c.tls_enabled=true;return c;
}
bool equalConfig(const meshtastic_ModuleConfig_MQTTConfig &a,const meshtastic_ModuleConfig_MQTTConfig &b){
  return a.enabled==b.enabled&&!strcmp(a.address,b.address)&&!strcmp(a.username,b.username)&&!strcmp(a.password,b.password)&&
    a.encryption_enabled==b.encryption_enabled&&a.json_enabled==b.json_enabled&&a.tls_enabled==b.tls_enabled&&
    !strcmp(a.root,b.root)&&a.proxy_to_client_enabled==b.proxy_to_client_enabled&&a.map_reporting_enabled==b.map_reporting_enabled&&
    a.has_map_report_settings==b.has_map_report_settings&&
    a.map_report_settings.publish_interval_secs==b.map_report_settings.publish_interval_secs&&
    a.map_report_settings.position_precision==b.map_report_settings.position_precision&&
    a.map_report_settings.should_report_location==b.map_report_settings.should_report_location;
}
void recalcOuter(Row &r,uint32_t generation){put32(r.bytes+4,generation);put32(r.bytes+RecordSize-4,crc32(r.bytes,RecordSize-4));}
void testTypedMappingAndUnsupported(){
  auto full=storedConfig();full.enabled=true;full.json_enabled=true;full.proxy_to_client_enabled=true;full.map_reporting_enabled=true;
  uint8_t wire[512]{};auto stream=pb_ostream_from_buffer(wire,sizeof wire);
  assert(pb_encode(&stream,meshtastic_ModuleConfig_MQTTConfig_fields,&full));
  meshtastic_ModuleConfig_MQTTConfig decoded=meshtastic_ModuleConfig_MQTTConfig_init_zero;auto input=pb_istream_from_buffer(wire,stream.bytes_written);
  assert(pb_decode(&input,meshtastic_ModuleConfig_MQTTConfig_fields,&decoded)&&input.bytes_left==0&&equalConfig(full,decoded));
  MqttGatewaySettings mapped;assert(MqttGatewayConfigMapping::fromConfig(decoded,mapped)&&mapped.valid());
  assert(MqttGatewayConfigMapping::operationalStatus(mapped,true,true)==0); // Stored JSON bit is inert.
  meshtastic_ModuleConfig_MQTTConfig exported{};assert(MqttGatewayConfigMapping::toConfig(mapped,exported)&&equalConfig(full,exported));
  Backend backend;MqttGatewayJournal journal;assert(journal.load(backend)==0&&journal.state()==MqttGatewayJournal::State::Fresh);
  assert(journal.save(backend,mapped,true,true)==0&&backend.writes==1);
  assert(MqttGatewayJournal::RecordSize==596&&sizeof(MqttGatewaySettings)==MqttGatewaySettings::Size);
  auto json=storedConfig();json.enabled=true;json.tls_enabled=false;json.json_enabled=true;
  memset(json.address,0,sizeof(json.address));MqttGatewaySettings inertJson;
  assert(MqttGatewayConfigMapping::fromConfig(json,inertJson));
  assert(MqttGatewayConfigMapping::operationalStatus(inertJson,true,false)==0);
  meshtastic_ModuleConfig_MQTTConfig jsonRoundTrip{};
  assert(MqttGatewayConfigMapping::toConfig(inertJson,jsonRoundTrip));
  assert(jsonRoundTrip.json_enabled&&!jsonRoundTrip.address[0]);
  mt::Config jsonChannels;assert(jsonChannels.valid());mt::mqtt::BootSnapshot jsonSnapshot;
  assert(jsonSnapshot.capture(inertJson,0x12345678u,true,false,jsonChannels));
  assert(jsonSnapshot.settings().enabled&&!jsonSnapshot.settings().proxyToClient);
  assert(!strcmp(jsonSnapshot.settings().host,"mqtt.meshtastic.org")&&jsonSnapshot.settings().port==1883);
  Backend jsonBackend;MqttGatewayJournal jsonJournal;
  assert(jsonJournal.load(jsonBackend)==0&&jsonJournal.save(jsonBackend,inertJson,true,false)==0);
  MqttGatewayJournal jsonReload;assert(jsonReload.load(jsonBackend)==0);
  assert(MqttGatewayConfigMapping::toConfig(jsonReload.config(),jsonRoundTrip)&&jsonRoundTrip.json_enabled&&!jsonRoundTrip.address[0]);
  auto supported=storedConfig();supported.enabled=true;supported.tls_enabled=false;strcpy(supported.address,"broker.native.invalid");
  supported.proxy_to_client_enabled=true;supported.map_reporting_enabled=true;supported.has_map_report_settings=true;
  supported.map_report_settings.publish_interval_secs=43200;
  supported.map_report_settings.position_precision=14;
  supported.map_report_settings.should_report_location=true;
  MqttGatewaySettings m116;assert(MqttGatewayConfigMapping::fromConfig(supported,m116));
  assert(m116.proxyEnabled()&&m116.mapReportingEnabled()&&m116.mapSettingsConfigured()&&
         m116.mapLocationOptedIn()&&m116.mapPublishIntervalSeconds()==43200&&
         m116.mapPositionPrecision()==14);
  assert(MqttGatewayConfigMapping::operationalStatus(m116)==0);
  assert(MqttGatewayConfigMapping::operationalStatus(m116,true,false)==0);
  Backend mapStoreBackend;MqttGatewayJournal mapStore;
  assert(mapStore.load(mapStoreBackend)==0&&mapStore.save(mapStoreBackend,m116,true,false)==0);
  MqttGatewayJournal mapRestored;assert(mapRestored.load(mapStoreBackend)==0);
  meshtastic_ModuleConfig_MQTTConfig mapRoundTrip{};
  assert(MqttGatewayConfigMapping::toConfig(mapRestored.config(),mapRoundTrip)&&equalConfig(supported,mapRoundTrip));
  auto delegated=supported;delegated.proxy_to_client_enabled=true;delegated.tls_enabled=true;
  MqttGatewaySettings phoneTls;assert(MqttGatewayConfigMapping::fromConfig(delegated,phoneTls));
  assert(MqttGatewayConfigMapping::operationalStatus(phoneTls,false,false)==0);
  assert(MqttGatewayConfigMapping::operationalStatus(phoneTls,true,false)==0);
  delegated.proxy_to_client_enabled=false;MqttGatewaySettings directTls;
  assert(MqttGatewayConfigMapping::fromConfig(delegated,directTls));
  assert(MqttGatewayConfigMapping::operationalStatus(directTls,true,false)==-ENOTSUP);
  delegated.proxy_to_client_enabled=true;memset(delegated.address,0,sizeof(delegated.address));
  assert(MqttGatewayConfigMapping::fromConfig(delegated,phoneTls));
  assert(MqttGatewayConfigMapping::operationalStatus(phoneTls,false,false)==0);
  mt::Config phoneChannels;assert(phoneChannels.valid());mt::mqtt::BootSnapshot phoneDefault;
  assert(phoneDefault.capture(phoneTls,0x12345678u,false,false,phoneChannels));
  assert(phoneDefault.proxyEnabled()&&phoneDefault.settings().port==8883);
  assert(!strcmp(phoneDefault.settings().host,"mqtt.meshtastic.org"));
  auto enabled=storedConfig();enabled.enabled=true;memset(enabled.address,0,sizeof(enabled.address));strcpy(enabled.address,"mqtt.example.test");enabled.tls_enabled=true;
  MqttGatewaySettings gated;assert(MqttGatewayConfigMapping::fromConfig(enabled,gated));
  assert(MqttGatewayConfigMapping::operationalStatus(gated)==-ENOTSUP);
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,false)==-ENOTSUP);
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,true)==0);
  enabled.tls_enabled=false;assert(MqttGatewayConfigMapping::fromConfig(enabled,gated));
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,false)==0);
  enabled.tls_enabled=true;assert(MqttGatewayConfigMapping::fromConfig(enabled,gated));
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,false)==-ENOTSUP);
  enabled.tls_enabled=true;memset(enabled.address,0,sizeof(enabled.address));assert(MqttGatewayConfigMapping::fromConfig(enabled,gated));
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,false)==-ENOTSUP);
  assert(MqttGatewayConfigMapping::operationalStatus(gated,true,true)==0);
  mt::Config emptyAddressChannels;assert(emptyAddressChannels.valid());mt::mqtt::BootSnapshot tlsDefault;
  assert(tlsDefault.capture(gated,0x12345678u,true,true,emptyAddressChannels));
  assert(!strcmp(tlsDefault.settings().host,"mqtt.meshtastic.org")&&tlsDefault.settings().port==8883);
  assert(tlsDefault.settings().tls&&tlsDefault.settings().trust.enabled&&tlsDefault.settings().trust.securityTag==mt::mqtt::BootSnapshot::SecurityTag);
  meshtastic_ModuleConfig_MQTTConfig tlsRoundTrip{};
  assert(MqttGatewayConfigMapping::toConfig(gated,tlsRoundTrip)&&tlsRoundTrip.tls_enabled&&!tlsRoundTrip.address[0]);
  puts("PASS MQTTConfig round-trip; deprecated JSON bit is inert; empty address resolves to stock TLS/plain defaults; direct TLS trust gate remains explicit");
}
void testStringReplacementAndClearing(){
  auto original=storedConfig();MqttGatewaySettings current;
  assert(MqttGatewayConfigMapping::fromConfig(original,current)&&current.valid());
  auto shorter=original;
  memset(shorter.address,0,sizeof(shorter.address));strcpy(shorter.address,"b");
  memset(shorter.username,0,sizeof(shorter.username));strcpy(shorter.username,"u");
  memset(shorter.password,0,sizeof(shorter.password));
  memset(shorter.root,0,sizeof(shorter.root));strcpy(shorter.root,"r");
  MqttGatewaySettings staged;assert(MqttGatewayConfigMapping::fromConfig(shorter,current,staged)&&staged.valid());
  meshtastic_ModuleConfig_MQTTConfig roundTrip{};
  assert(MqttGatewayConfigMapping::toConfig(staged,roundTrip)&&equalConfig(shorter,roundTrip));

  Backend backend;MqttGatewayJournal journal;assert(!journal.load(backend));
  assert(!journal.save(backend,current));
  assert(!journal.save(backend,staged));
  MqttGatewayJournal restored;assert(!restored.load(backend)&&restored.config().valid());
  assert(MqttGatewayConfigMapping::toConfig(restored.config(),roundTrip)&&equalConfig(shorter,roundTrip));

  auto cleared=shorter;memset(cleared.address,0,sizeof(cleared.address));
  memset(cleared.username,0,sizeof(cleared.username));memset(cleared.password,0,sizeof(cleared.password));
  memset(cleared.root,0,sizeof(cleared.root));
  MqttGatewaySettings empty;assert(MqttGatewayConfigMapping::fromConfig(cleared,restored.config(),empty)&&empty.valid());
  assert(!journal.save(backend,empty));
  MqttGatewayJournal restoredEmpty;assert(!restoredEmpty.load(backend)&&restoredEmpty.config().valid());
  assert(MqttGatewayConfigMapping::toConfig(restoredEmpty.config(),roundTrip)&&equalConfig(cleared,roundTrip));
  for(size_t i=0;i<64;++i){assert(!empty.bytes[MqttGatewaySettings::Address+i]);assert(!empty.bytes[MqttGatewaySettings::Username+i]);}
  for(size_t i=0;i<32;++i){assert(!empty.bytes[MqttGatewaySettings::Password+i]);assert(!empty.bytes[MqttGatewaySettings::Root+i]);}
  puts("PASS shorter and cleared address/user/password/root replace old buffers and survive journal reload");
}
void testConsentsBindExactChannelIdentity(){
  mt::Config config;assert(config.valid()&&config.channelConfigured());
  MqttGatewaySettings s;assert(!s.configOkToMqtt()&&!s.channelUplinkEnabled(config,0));
  assert(s.setConfigOkToMqtt(true)&&s.configOkToMqtt());
  assert(s.setChannelUplink(config,0,true)&&s.channelUplinkEnabled(config,0));
  auto renamed=config;assert(renamed.setPrimaryName("Private"));
  assert(!s.channelUplinkEnabled(renamed,0));
  assert(s.setChannelUplink(renamed,0,true)&&s.channelUplinkEnabled(renamed,0));
  assert(!s.channelUplinkEnabled(config,0));
  const uint8_t key[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  mt::ChannelRecord sub;assert(sub.set(2,"Sub",key,sizeof(key))&&config.setSecondaryChannel(1,sub));
  assert(s.setChannelUplink(config,1,true)&&s.channelUplinkEnabled(config,1));
  auto reassigned=config;mt::ChannelRecord other;const uint8_t key2[16]={16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
  assert(other.set(2,"Other",key2,sizeof(key2))&&reassigned.setSecondaryChannel(1,other));
  assert(!s.channelUplinkEnabled(reassigned,1)&&!s.channelUplinkEnabled(config,2));
  auto module=storedConfig();MqttGatewaySettings remapped;
  assert(MqttGatewayConfigMapping::fromConfig(module,s,remapped));
  assert(remapped.configOkToMqtt()&&remapped.channelUplinkEnabled(config,1));
  assert(remapped.setChannelUplink(config,1,false)&&!remapped.channelUplinkEnabled(config,1));
  puts("PASS consent flag and per-slot uplink consent bind exact role/name/full normalized key across reassignment");
}
void testBootSnapshotFeatureDemand(){
  mt::Config channels;assert(channels.valid());
  auto module=storedConfig();module.enabled=true;module.tls_enabled=false;strcpy(module.address,"broker.native.invalid");
  MqttGatewaySettings config;assert(MqttGatewayConfigMapping::fromConfig(module,config));
  mt::mqtt::BootSnapshot snapshot;
  assert(snapshot.capture(config,0x12345678u,true,false,channels));
  assert(!snapshot.settings().wantsLink&&!snapshot.settings().proxyToClient);

  module.map_reporting_enabled=true;module.has_map_report_settings=true;
  module.map_report_settings.publish_interval_secs=3600;
  module.map_report_settings.position_precision=14;
  module.map_report_settings.should_report_location=false; // still stock link demand
  assert(MqttGatewayConfigMapping::fromConfig(module,config));
  assert(snapshot.capture(config,0x12345678u,true,false,channels));
  assert(snapshot.settings().wantsLink&&!snapshot.settings().proxyToClient);

  module.proxy_to_client_enabled=true;
  assert(MqttGatewayConfigMapping::fromConfig(module,config));
  assert(snapshot.capture(config,0x12345678u,true,false,channels));
  assert(snapshot.settings().wantsLink&&snapshot.settings().proxyToClient&&snapshot.proxyEnabled());

  auto uplinkSettings=storedConfig();uplinkSettings.enabled=true;uplinkSettings.tls_enabled=false;
  strcpy(uplinkSettings.address,"broker.native.invalid");
  assert(MqttGatewayConfigMapping::fromConfig(uplinkSettings,config));
  assert(config.setChannelUplink(channels,0,true));
  assert(snapshot.capture(config,0x12345678u,true,false,channels));
  assert(snapshot.settings().wantsLink);
}
void testDefaultBrokerPortCompatibility(){
  mt::Config channels;assert(channels.valid());
  auto input=storedConfig();input.enabled=true;input.tls_enabled=false;
  memset(input.address,0,sizeof(input.address));
  MqttGatewaySettings settings;assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  assert(settings.admissionStatus(true,false)==0);
  mt::mqtt::BootSnapshot snapshot;
  assert(snapshot.capture(settings,0x12345678u,true,false,channels));
  assert(!strcmp(snapshot.settings().host,"mqtt.meshtastic.org")&&snapshot.settings().port==1883);
  meshtastic_ModuleConfig_MQTTConfig emptyPlainRoundTrip{};
  assert(MqttGatewayConfigMapping::toConfig(settings,emptyPlainRoundTrip)&&!emptyPlainRoundTrip.address[0]);

  strcpy(input.address,"mqtt.meshtastic.org");
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  mt::mqtt::BootSnapshot explicitHost;
  assert(explicitHost.capture(settings,0x12345678u,true,false,channels));
  assert(!strcmp(explicitHost.settings().host,"mqtt.meshtastic.org")&&explicitHost.settings().port==1883);

  input.tls_enabled=true;
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  mt::mqtt::BootSnapshot implicitTlsPort;
  assert(implicitTlsPort.capture(settings,0x12345678u,true,true,channels));
  assert(implicitTlsPort.settings().port==8883);

  // Stock allows either standard explicit port independent of TLS state.
  const char *ports[]={"1883","8883"};
  for(const char *port : ports)for(unsigned tls=0;tls<2;++tls){
    memset(input.address,0,sizeof(input.address));
    snprintf(input.address,sizeof(input.address),"mqtt.meshtastic.org:%s",port);
    input.tls_enabled=tls!=0;
    assert(MqttGatewayConfigMapping::fromConfig(input,settings));
    assert(settings.admissionStatus(true,input.tls_enabled)==0);
    mt::mqtt::BootSnapshot explicitPort;
    assert(explicitPort.capture(settings,0x12345678u,true,input.tls_enabled,channels));
    assert(explicitPort.settings().port==unsigned(!strcmp(port,"8883")?8883:1883));
  }

  memset(input.address,0,sizeof(input.address));strcpy(input.address,"mqtt.meshtastic.org:1884");
  input.enabled=true;input.tls_enabled=false;
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  assert(settings.admissionStatus(true,true)==-EINVAL);
  mt::mqtt::BootSnapshot refusedDefault;
  assert(!refusedDefault.capture(settings,0x12345678u,true,true,channels));

  // The same rule applies to phone proxy mode; it does not transfer or weaken
  // the default broker's allowed-port policy.
  input.proxy_to_client_enabled=true;input.tls_enabled=true;
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  assert(settings.admissionStatus(false,false)==-EINVAL);
  memset(input.address,0,sizeof(input.address));strcpy(input.address,"mqtt.meshtastic.org:8883");
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  assert(settings.admissionStatus(false,false)==0);
  mt::mqtt::BootSnapshot proxySnapshot;
  assert(proxySnapshot.capture(settings,0x12345678u,false,false,channels));
  assert(proxySnapshot.proxyEnabled()&&proxySnapshot.settings().port==8883);

  // Custom brokers retain stock's unrestricted custom-port behavior.
  memset(input.address,0,sizeof(input.address));strcpy(input.address,"broker.example:65535");
  input.proxy_to_client_enabled=false;input.tls_enabled=false;
  assert(MqttGatewayConfigMapping::fromConfig(input,settings));
  assert(settings.admissionStatus(true,false)==0);
  mt::mqtt::BootSnapshot custom;
  assert(custom.capture(settings,0x12345678u,true,false,channels));
  assert(!strcmp(custom.settings().host,"broker.example")&&custom.settings().port==65535);
  puts("PASS stock default broker port restriction, TLS/proxy handling, and custom broker port preservation");
}
void testJournalRoundTripAndIsolation(){
  Backend b;Row *legacy=nullptr,*modules=nullptr,*wifi=nullptr;
  for(auto &r:b.rows)if(!r.key[0]){
    if(!legacy){legacy=&r;strcpy(r.key,"mtbase/state0");}
    else if(!modules){modules=&r;strcpy(r.key,"mtmod/state0");}
    else if(!wifi){wifi=&r;strcpy(r.key,"mtwifi/state0");break;}
  }
  assert(legacy&&modules&&wifi);
  Row *untouched[]={legacy,modules,wifi};
  for(Row *r:untouched){r->size=4;memcpy(r->bytes,"keep",4);}
  MqttGatewayJournal j;assert(j.load(b)==0&&j.ready()&&j.state()==MqttGatewayJournal::State::Fresh&&j.activeSlot()==-1&&j.generation()==0);
  assert(!j.config().enabled()&&j.config().valid()&&b.writes==0);
  MqttGatewaySettings candidate;auto config=storedConfig();assert(MqttGatewayConfigMapping::fromConfig(config,candidate));
  assert(candidate.admissionStatus()==0&&j.save(b,candidate)==0&&j.activeSlot()==0&&j.generation()==1);
  Row *raw=b.find("mtmqtt/state0");assert(raw&&raw->size==RecordSize&&raw->bytes[0]=='Z'&&raw->bytes[1]=='T'&&raw->bytes[2]=='M'&&raw->bytes[3]==2);
  assert(raw->bytes[8]==uint8_t(MqttGatewaySettings::Size)&&raw->bytes[9]==uint8_t(MqttGatewaySettings::Size>>8)&&raw->bytes[10]==0&&raw->bytes[11]==0);
  MqttGatewayJournal reboot;assert(reboot.load(b)==0&&reboot.state()==MqttGatewayJournal::State::Loaded&&reboot.generation()==1&&reboot.activeSlot()==0);
  meshtastic_ModuleConfig_MQTTConfig back{};assert(MqttGatewayConfigMapping::toConfig(reboot.config(),back)&&equalConfig(config,back));
  auto updated=config;updated.tls_enabled=false;MqttGatewaySettings next;assert(MqttGatewayConfigMapping::fromConfig(updated,next));
  mt::Config channelConfig;assert(next.setConfigOkToMqtt(true)&&next.setChannelUplink(channelConfig,0,true));
  assert(reboot.save(b,next,true,true)==0&&reboot.activeSlot()==1&&reboot.generation()==2);
  MqttGatewayJournal newest;assert(newest.load(b)==0&&newest.state()==MqttGatewayJournal::State::Loaded&&newest.activeSlot()==1&&newest.config().valid());
  assert(newest.config().configOkToMqtt()&&newest.config().channelUplinkEnabled(channelConfig,0));
  // Isolated new namespace: pre-existing mtbase/mtmod entries remain untouched.
  for(size_t i=0;i<b.reads;++i)assert(!strcmp(b.readKeys[i],"mtmqtt/state0")||!strcmp(b.readKeys[i],"mtmqtt/state1"));
  assert(b.touchedOnlyMqtt());
  for(Row *r:untouched)assert(r->size==4&&!memcmp(r->bytes,"keep",4));
  puts("PASS fresh default-off namespaced dual-slot save, exact readback, and reload");
}
void testRecoveryCorruptionAndAmbiguity(){
  Backend b;MqttGatewayJournal j;assert(j.load(b)==0);auto a=storedConfig();MqttGatewaySettings sa;assert(MqttGatewayConfigMapping::fromConfig(a,sa));assert(j.save(b,sa)==0);
  a.tls_enabled=false;MqttGatewaySettings sb;assert(MqttGatewayConfigMapping::fromConfig(a,sb));assert(j.save(b,sb)==0);
  Row *slot0=b.find("mtmqtt/state0"),*slot1=b.find("mtmqtt/state1");assert(slot0&&slot1);
  slot1->bytes[RecordSize-1]^=0x80;MqttGatewayJournal recovered;assert(recovered.load(b)==0&&recovered.state()==MqttGatewayJournal::State::Recovered&&recovered.generation()==1);
  // Both valid, equal generations with distinct records have no safe ordering.
  MqttGatewayJournal repair;assert(repair.load(b)==0&&repair.save(b,sb)==0);slot0=b.find("mtmqtt/state0");slot1=b.find("mtmqtt/state1");
  recalcOuter(*slot1,get32(slot0->bytes+4));MqttGatewayJournal ambiguous;assert(ambiguous.load(b)==-EINVAL&&ambiguous.state()==MqttGatewayJournal::State::Invalid);
  // Half-range generation distance is likewise ambiguous.
  recalcOuter(*slot1,get32(slot0->bytes+4)+0x80000000u);MqttGatewayJournal halfRange;assert(halfRange.load(b)==-EINVAL&&halfRange.state()==MqttGatewayJournal::State::Invalid);
  Row versioned;strcpy(versioned.key,"mtmqtt/state0");versioned.size=RecordSize;memcpy(versioned.bytes,slot0->bytes,RecordSize);versioned.bytes[3]=1;recalcOuter(versioned,get32(versioned.bytes+4));
  MqttGatewayJournal wrongVersion;Backend wrongVersionBackend;wrongVersionBackend.rows[0]=versioned;
  assert(wrongVersion.load(wrongVersionBackend)==-EINVAL&&wrongVersion.state()==MqttGatewayJournal::State::Invalid);
  slot0->bytes[RecordSize-1]^=1;slot1->bytes[RecordSize-1]^=1;MqttGatewayJournal invalid;assert(invalid.load(b)==-EINVAL&&invalid.state()==MqttGatewayJournal::State::Invalid);
  puts("PASS corrupt-slot recovery, same-generation and half-range ambiguity refusal, and all-corrupt refusal");
}
void testRecoveredConsentBlocksRemotePolicyUntilLocalRepair(){
  Backend backend;MqttGatewayJournal seed;assert(seed.load(backend)==0);
  mt::Config channelConfig;assert(channelConfig.valid());
  auto input=storedConfig();memset(input.address,0,sizeof(input.address));
  strcpy(input.address,"mqtt.meshtastic.org");memset(input.root,0,sizeof(input.root));strcpy(input.root,"msh");
  MqttGatewaySettings older;assert(MqttGatewayConfigMapping::fromConfig(input,older));
  assert(older.setChannelUplink(channelConfig,0,true));
  assert(seed.save(backend,older)==0&&seed.generation()==1&&seed.activeSlot()==0);
  MqttGatewaySettings newer=older;assert(newer.setChannelUplink(channelConfig,0,false));
  assert(seed.save(backend,newer)==0&&seed.generation()==2&&seed.activeSlot()==1);
  Row *newerSlot=backend.find("mtmqtt/state1");assert(newerSlot);
  newerSlot->bytes[RecordSize-1]^=0x80;

  TestRadio radio;mt::Service service{radio,true,0x12345678u,81,true,false};
  mt::SettingsJournal coreJournal;mt::SettingsEdit core{service,coreJournal,backend};
  MqttGatewayJournal recovered;assert(recovered.load(backend)==0&&
      recovered.state()==MqttGatewayJournal::State::Recovered&&recovered.generation()==1&&
      recovered.activeSlot()==0&&recovered.config().channelUplinkEnabled(channelConfig,0));
  mt::network::MqttGatewayEdit edit{service,recovered,backend};
  mt::network::MqttGatewayAdmin admin{edit,core};assert(admin.loaded());
  assert(!admin.remotePolicyWritable());

  const size_t writes=backend.writes;const uint32_t generation=recovered.generation();
  assert(admin.saveRemoteChannelUplink(channelConfig,0,false,true,100)==-EACCES);
  assert(admin.applyRemoteRegionRootTopic("US",2,false,101,false)==-EACCES);
  meshtastic_ModuleConfig_MQTTConfig remoteModule{};
  assert(MqttGatewayConfigMapping::toConfig(recovered.config(),remoteModule));
  strcpy(remoteModule.root,"msh/US");
  assert(admin.saveRemote(remoteModule,true,false,false,102)==-EACCES);
  assert(backend.writes==writes&&recovered.state()==MqttGatewayJournal::State::Recovered&&
      recovered.generation()==generation&&recovered.activeSlot()==0&&
      recovered.config().channelUplinkEnabled(channelConfig,0));

  // A deliberate local edit still repairs the newer slot through the normal
  // journal path; the RF-only guard does not remove owner recovery.
  assert(admin.saveChannelUplink(channelConfig,0,false,true,103)==0);
  assert(recovered.state()==MqttGatewayJournal::State::Loaded&&recovered.generation()==2&&
      recovered.activeSlot()==1&&!recovered.config().channelUplinkEnabled(channelConfig,0));
  puts("PASS recovered MQTT consent holds remote edits without writes and allows explicit local repair");
}
void testWriteFailuresAndMalformedFields(){
  auto c=storedConfig();MqttGatewaySettings s;assert(MqttGatewayConfigMapping::fromConfig(c,s));
  Backend torn;MqttGatewayJournal t;assert(t.load(torn)==0);torn.tearWrite=true;assert(t.save(torn,s)==-EIO&&t.state()==MqttGatewayJournal::State::IoError);assert(t.save(torn,s)==-EACCES);
  Backend altered;MqttGatewayJournal a;assert(a.load(altered)==0);altered.alterReadback=true;assert(a.save(altered,s)==-EIO&&a.state()==MqttGatewayJournal::State::IoError);assert(a.save(altered,s)==-EACCES);
  Backend failed;MqttGatewayJournal f;assert(f.load(failed)==0);failed.writeError=-EIO;assert(f.save(failed,s)==-EIO&&f.state()==MqttGatewayJournal::State::IoError);
  auto malformed=c;memset(malformed.address,'x',sizeof malformed.address);assert(!MqttGatewayConfigMapping::fromConfig(malformed,s));
  MqttGatewaySettings bad=s;bad.bytes[MqttGatewaySettings::AddressLength]=64;assert(!bad.valid());
  bad=s;bad.bytes[MqttGatewaySettings::Address+2]^=1;assert(!bad.valid());
  puts("PASS torn/write/readback poison, refusal after uncertain commit, and canonical input/CRC validation");
}
}
int main(){
  static_assert(sizeof(MqttGatewaySettings)==580,"settings and exact consent bindings are fixed-size");
  testTypedMappingAndUnsupported();testStringReplacementAndClearing();testConsentsBindExactChannelIdentity();testBootSnapshotFeatureDemand();testDefaultBrokerPortCompatibility();testJournalRoundTripAndIsolation();testRecoveryCorruptionAndAmbiguity();testRecoveredConsentBlocksRemotePolicyUntilLocalRepair();testWriteFailuresAndMalformedFields();
  puts("PASS MqttGatewaySettings native cases");
}
