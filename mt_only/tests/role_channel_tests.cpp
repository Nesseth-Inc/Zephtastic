// SPDX-License-Identifier: GPL-3.0-only
// Actual Config/journal/Service/Companion with synthetic native I/O only.
#include "device_config_fixture.h"
#include "known_channel_fixture.h"
#include "stock_routing_timing/long_fast.h"
#include "ChannelName.h"
#include "CorePorts.h"
#include "pki/PkiSigner.h"
#include "monocypher.h"
#include <cstddef>
#include <cstdint>
static void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
static auto deviceGetter(Fixture &f,bool ble) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  a.get_config_request=meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;f.admin(a,ble);
  return (ble?f.ble:f.usb).reply().get_config_response.payload_variant.device;
}
struct RelayRadio:Radio {
  std::vector<std::vector<uint8_t>> frames;const uint8_t *borrowed=nullptr;size_t length=0;
  int result=0;unsigned attempts=0;
  int send(uint8_t *p,size_t n)override{++attempts;if(result)return result;borrowed=p;length=n;frames.emplace_back(p,p+n);return Radio::send(p,n);}
};
struct Side {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  RelayRadio radio;mt::Service s;
  Side(uint32_t node,unsigned role=0,unsigned mode=0,const char *name=""):s(radio,true,node,81,true,false) {
    knownChannelClock.bind(s);
    mt::Config c;c.bytes[4]=1;assert(c.setDeviceRole(role,mode)&&c.setPrimaryName(name));c.seal();
    assert(s.apply(c)&&s.start(false,0)&&s.armEndpoint(true));
  }
  void receive(const std::vector<uint8_t> &p,uint32_t now=1000){s.rx(p.data(),p.size(),3,now,0);}
  void complete(uint32_t now=11000){radio.done=true;s.poll(now,0);}
};
static std::vector<uint8_t> packet(Side &source,uint32_t to,uint32_t id=201,unsigned hop=2,unsigned port=1) {
  uint8_t data[]={8,uint8_t(port),18,3,'b','a','t',72,0};uint8_t frame[255]{};size_t n=0;
  assert(port<128&&source.s.codec.encodeZeroHopData(source.s.route.identity(),to,id,data,sizeof(data),frame,sizeof(frame),n));
  frame[12]=uint8_t(hop|(hop<<5));return {frame,frame+n};
}
namespace PublicPolicy {
struct Keys:mt::pki::KeyProvider {
 uint8_t scalar[32]{};mt::pki::IdentityBinding binding;mt::pki::KeyRef ref{1,1};
 Keys(){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(17+i*13);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;binding.generation=1;crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);}
 bool active(mt::pki::KeyRef v)const override{return v.slot==ref.slot&&v.generation==ref.generation;}
 bool withPrivate(mt::pki::KeyRef v,mt::pki::PrivateKeyOperation&o)override{return active(v)&&o.apply(scalar);}
};
struct Hedge:mt::pki::SigningEntropy{bool hedge(uint8_t out[32])override{for(unsigned i=0;i<32;++i)out[i]=uint8_t(91+i*7);return true;}};
static uint32_t snapshot(const mt::NodeCache &nodes){return mt::Config::checksum(reinterpret_cast<const uint8_t*>(&nodes),sizeof(nodes));}
static std::vector<uint8_t> frame(Fixture &f,Keys &keys,unsigned port,bool signedNode){
 uint8_t wire[255]{};size_t n=0;
 if(signedNode){Hedge entropy;mt::pki::NodeInfoProfile p;strcpy(p.longName,"Synthetic policy signer");strcpy(p.shortName,"POL");p.hardwareModel=81;mt::pki::SigningEnvelope envelope;envelope.hasBitfield=true;mt::pki::SignedData d;
  assert(mt::pki::PkiSigner::bootstrap(keys,keys.ref,keys.binding,entropy,901,0xffffffffu,p,envelope,d)==mt::pki::SigningResult::Ok);
  assert(f.service.codec.encodeSignedNodeInfo(d.from,0xffffffffu,901,d.bytes,d.size,wire,sizeof(wire),n));
 }else{meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;
  if(port==36){const uint8_t payload[]={10,3,'a','b','c'};d.payload.size=sizeof(payload);memcpy(d.payload.bytes,payload,sizeof(payload));}
  else{d.payload.size=3;memcpy(d.payload.bytes,"abc",3);}
  uint8_t data[239]{};auto out=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&out,meshtastic_Data_fields,&d));
  assert(f.service.codec.encodeZeroHopData(keys.binding.node,0xffffffffu,901,data,out.bytes_written,wire,sizeof(wire),n));
 }
 wire[12]=uint8_t(1|(1<<5));wire[15]=uint8_t(keys.binding.node);return {wire,wire+n};
}
static unsigned packets(const Host&h,unsigned port){unsigned n=0;for(const auto&m:h.out)n+=m.which_payload_variant==meshtastic_FromRadio_packet_tag&&unsigned(m.packet.decoded.portnum)==port;return n;}
static void run(unsigned mode,unsigned port,bool known,bool signedNode,bool expectFixed){
 Fixture f;f.provision();f.service.stop();auto cfg=f.service.config;assert(cfg.setDeviceRole(0,mode));assert(f.service.apply(cfg)&&f.service.start(false,0)&&f.service.armEndpoint(true));Keys keys;
 if(known){mt::NodeCache::Entry e{};e.identity(keys.binding.node,81,1);assert(mt::NodeCache::Entry::text(e.bytes+e.Name,40,"Synthetic known",16));assert(f.service.nodes.accept(e,Node,1,0)>=0);}
 meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=17;f.input(hello);f.input(hello,true);f.usb.out.clear();f.ble.out.clear();
 const auto bytes=frame(f,keys,port,signedNode);const auto before=snapshot(f.service.nodes);const auto countBefore=f.service.nodes.count();const auto keysBefore=f.service.signature.keyCount(1000);
 f.service.rx(bytes.data(),bytes.size(),3,1000,0);const auto serviceOnly=snapshot(f.service.nodes);const auto serviceKeys=f.service.signature.keyCount(1000);
 f.c.receive(bytes.data(),bytes.size(),-71,3,1000,1792660000u,true,12,true,mt::Companion::WorkspaceProof{true});const auto afterUsb=snapshot(f.service.nodes);const auto usbKeys=f.service.signature.keyCount(1000);
 f.b.receive(bytes.data(),bytes.size(),-71,3,1000,1792660000u,true,12,true,mt::Companion::WorkspaceProof{true});const auto afterBoth=snapshot(f.service.nodes);const auto bothKeys=f.service.signature.keyCount(1000);
 const auto usbPackets=packets(f.usb,port),blePackets=packets(f.ble,port);const int slot=f.service.nodes.find(keys.binding.node);const bool status=slot>=0&&f.service.nodes.get(unsigned(slot))->status.present();
 const bool blocked=(mode==3&&!known)||(mode==5&&port!=1);assert(!blocked||(before==serviceOnly&&serviceKeys==keysBefore));
 if(blocked&&expectFixed)assert(!usbPackets&&!blePackets&&afterBoth==before&&bothKeys==keysBefore);
 else {assert(usbPackets==1&&blePackets==1);if(signedNode)assert(bothKeys==1);if(port==36)assert(status);}
 std::cout<<"{\"mode\":"<<mode<<",\"port\":"<<port<<",\"known\":"<<known<<",\"policy_should_block\":"<<blocked<<",\"node_count_before\":"<<countBefore<<",\"node_count_after\":"<<f.service.nodes.count()<<",\"NodeCache_raw_crc16_before\":"<<before<<",\"NodeCache_raw_crc16_after_service\":"<<serviceOnly<<",\"NodeCache_raw_crc16_after_usb\":"<<afterUsb<<",\"NodeCache_raw_crc16_after_both\":"<<afterBoth<<",\"signature_keys_before\":"<<keysBefore<<",\"signature_keys_after_service\":"<<serviceKeys<<",\"signature_keys_after_usb\":"<<usbKeys<<",\"signature_keys_after_both\":"<<bothKeys<<",\"USB_packets\":"<<usbPackets<<",\"BLE_packets\":"<<blePackets<<",\"status_present\":"<<status<<"}\n";
}
}
int main(){
  PublicPolicy::run(3,4,false,true,true);PublicPolicy::run(5,36,true,false,true);PublicPolicy::run(5,256,true,false,true);
  PublicPolicy::run(0,4,false,true,true);PublicPolicy::run(3,4,true,true,true);PublicPolicy::run(5,1,true,false,true);
  pass("independent-original-six-public-policy-adversaries-both-fanouts-cache-signer-state-and-positive-controls");
  for(bool ble:{false,true})for(unsigned role:{0u,1u,2u,5u,6u,7u,8u,9u,12u})for(unsigned mode:{0u,2u,3u,4u,5u}) {
    Fixture f;f.provision();assert(f.service.start(false,0));auto a=device();
    a.set_config.payload_variant.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(role);
    a.set_config.payload_variant.device.rebroadcast_mode=static_cast<meshtastic_Config_DeviceConfig_RebroadcastMode>(mode);
    f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0);
    const unsigned effective=(role==2||role==11)&&mode==4?0:mode;
    assert(f.service.receiving()&&f.service.config.role()==role&&f.service.config.rebroadcastMode()==effective);
    const auto read=deviceGetter(f,!ble);assert(unsigned(read.role)==role&&unsigned(read.rebroadcast_mode)==effective);
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
    assert(!f.radio.sends);
  }
  pass("all-role-mode-pairs-USB-BLE-replacement-opposite-readback-router-NONE-normalization-durable-reboot");
  for(unsigned alias:{3u,4u}){
    Fixture f;f.provision();auto a=device(7200);
    a.set_config.payload_variant.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(alias);
    f.admin(a,false,10);
    assert(f.usb.error()==0&&f.usb.deprecatedRoleSaves==1);
    assert(f.service.config.role()==0&&f.service.config.node().interval()==7200);
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
  }
  pass("deprecated ROUTER_CLIENT and REPEATER setters normalize to persisted CLIENT while remaining setter-only aliases");
  for(bool ble:{false,true})for(unsigned bad:{255u}) {
    Fixture f;f.provision();const auto before=f.service.config;auto a=device();
    const uint32_t raw=bad;memcpy(&a.set_config.payload_variant.device.role,&raw,sizeof(raw)<sizeof(a.set_config.payload_variant.device.role)?sizeof(raw):sizeof(a.set_config.payload_variant.device.role));
    f.admin(a,ble);assert((ble?f.ble:f.usb).error()!=0&&f.service.config.same(before)&&!f.radio.sends);
  }
  pass("unsupported-roles-cannot-be-acknowledged-or-change-durable-settings");
  for(bool ble:{false,true}) {
    Fixture f;f.provision();meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
    auto &c=a.set_channel;c.index=0;c.role=meshtastic_Channel_Role_PRIMARY;c.has_settings=true;
    c.settings.psk.size=1;c.settings.psk.bytes[0]=1;strcpy(c.settings.name,"BatchLink");
    f.admin(a,ble);assert((ble?f.ble:f.usb).error()==0&&f.service.config.channelConfigured());
    assert(!strcmp(f.service.config.primaryName(),"BatchLink")&&f.service.codec.channelHash()==f.service.config.node().bytes[4]);
    assert(f.service.codec.channelHash()!=8&&f.service.route.pending()==0);
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(f.service.config));
    a={};a.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;a.get_channel_request=1;f.admin(a,!ble);
    const auto read=(ble?f.usb:f.ble).reply().get_channel_response;
    assert(!strcmp(read.settings.name,"BatchLink")&&read.settings.psk.size==1&&read.settings.psk.bytes[0]==1);
    assert(!f.radio.sends);
  }
  pass("named-primary-alias-normalizes-route-codec-hash-and-roundtrips-USB-BLE-storage");
  {
    const uint8_t key[32]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17};
    for(size_t size:{16u,32u})for(const char *name:{"meshtastic","abcdefghijk"}) {
      uint8_t expected=0,out=0xee;for(const char *p=name;*p;++p)expected^=uint8_t(*p);
      for(size_t i=0;i<size;++i){expected^=key[i];}
      assert(expected&&mt::ChannelName::hash(name,key,size,out)&&out==expected);
    }
    for(const char *name:{"","abcdefghijkl","a\x01" "b","a\xff" "b"}) {
      uint8_t out=0xee;assert(!mt::ChannelName::hash(name,key,16,out)&&out==0xee);
    }
    uint8_t out=0xee,zeroHashKey[16]={0x41};assert(mt::ChannelName::hash("A",zeroHashKey,16,out)&&out==0);
    out=0xee;
    assert(!mt::ChannelName::hash(nullptr,key,16,out)&&out==0xee);
    assert(!mt::ChannelName::hash("A",nullptr,16,out)&&out==0xee);
    for(size_t n:{0u,1u,15u,17u,31u,33u})assert(!mt::ChannelName::hash("A",key,n,out)&&out==0xee);
    for(unsigned p=0;p<1024;++p){const bool expected=p==1||p==3||p==4||p==5||p==6||p==7||p==8||p==11||p==12||p==35||p==65||p==67||p==70;assert(mt::corePort(p)==expected);}
  }
  pass("independent-full-key-XOR-helper-boundaries-and-all1024-core-port-predicates");
  {
    Side a(0x11223344),b(0x22334455),c(0x33445566);const auto input=packet(a,c.s.route.identity());
    b.receive(input);assert(b.s.route.pending()==1);b.s.poll(10000,0);assert(b.radio.frames.size()==1&&b.s.active());
    const auto &wire=b.radio.frames[0];assert(wire.size()==input.size()&&!memcmp(wire.data(),input.data(),12));
    assert(!memcmp(wire.data()+16,input.data()+16,input.size()-16)&&(wire[12]&7)==1);
    assert(b.radio.length==wire.size()&&!memcmp(b.radio.borrowed,wire.data(),wire.size()));
    c.receive(wire,10200);bench::RouteMetadata m;assert(c.s.codec.decode(wire.data(),wire.size(),m)&&m.channelDecoded&&m.portnum==1&&m.payloadLength==3&&!memcmp(m.payload,"bat",3));
    b.complete();assert(b.s.sent==1&&!b.s.active()&&!b.s.route.pending());
  }
  pass("three-normal-Services-public-transit-native-borrow-completion-and-recipient-codec");
  constexpr uint32_t initialDue = 1000 + stock_long_fast::initialMs;
  constexpr uint32_t duplicateDue = 1200 + stock_long_fast::duplicateMs;
  for(unsigned role:{0u,1u,2u,11u,12u}) {
    Side a(0x11223344),b(0x22334455,role);auto original=packet(a,0xffffffffu);b.receive(original);
    assert(b.s.route.pending()==unsigned(role!=1));
    if(role==1)continue;
    auto duplicate=original;duplicate[12]=uint8_t(1|(2<<5));duplicate[15]=0x77;b.receive(duplicate,1200);
    assert(b.s.route.pending()==unsigned(role==2||role==11));
  }
  pass("CLIENT-dupe-cancels-ROUTER-and-ROUTER_LATE-retain-CLIENT-MUTE-never-relays");
  {
    Side a(0x11223344),late(0x22334455,11),router(0x33445566,2);
    auto p=packet(a,0xffffffffu,250,2);late.receive(p,1000);
    assert(late.s.route.pending()==1&&!late.s.route.next(initialDue - 1)&&late.s.route.next(initialDue));
    p=packet(a,0xffffffffu,251,2);router.receive(p,1000);
    assert(router.s.route.pending()==1&&router.s.route.next(1000));
  }
  pass("ROUTER_LATE keeps ordinary client-like initial delay while ROUTER uses early delay");
  auto favoriteNode=[](Side &side,uint32_t id,unsigned role) {
    mt::NodeCache::Entry e{};e.identity(id,81,role);
    assert(mt::NodeCache::Entry::text(e.bytes+e.Name,40,"Synthetic favorite",20));
    assert(side.s.nodes.accept(e,side.s.route.identity(),-20,1)>=0);
    assert(side.s.nodes.edit(id,side.s.route.identity(),mt::NodeCache::Edit::Favorite)==1);
  };
  auto placeholderFavorite=[](Side &side,uint32_t id) {
    assert(side.s.nodes.edit(id,side.s.route.identity(),mt::NodeCache::Edit::Ignore)==1);
    assert(side.s.nodes.edit(id,side.s.route.identity(),mt::NodeCache::Edit::Unignore)==1);
    assert(side.s.nodes.edit(id,side.s.route.identity(),mt::NodeCache::Edit::Favorite)==1);
    const int slot=side.s.nodes.find(id);const auto *e=slot<0?nullptr:side.s.nodes.get(unsigned(slot));
    assert(e&&e->placeholder()&&e->favorite());
  };
  for(bool favoriteSource:{true,false}) {
    Side a(0x11223344),b(0x22334455,12);const uint32_t destination=0x33445566;
    favoriteNode(b,favoriteSource?a.s.route.identity():destination,0);
    auto original=packet(a,destination,301);b.receive(original,1000);assert(b.s.route.pending()==1);
    auto duplicate=original;duplicate[12]=uint8_t((2u<<5)|1u);duplicate[15]=0x77;b.receive(duplicate,1200);
    assert(b.s.route.pending()==1&&!b.s.route.next(duplicateDue - 1));
    assert(b.s.route.next(duplicateDue));
  }
  {Side a(0x11223344),b(0x22334455,12);auto original=packet(a,0x33445566,302);
   b.receive(original,1000);assert(b.s.route.pending()==1);auto duplicate=original;
   duplicate[12]=uint8_t((2u<<5)|1u);duplicate[15]=0x77;b.receive(duplicate,1200);
   assert(b.s.route.pending()==0);}
  for(bool placeholder:{true,false}) {
    Side a(0x11223344),b(0x22334455,12);const uint32_t destination=0x33445566;
    if(placeholder)placeholderFavorite(b,destination);
    else {favoriteNode(b,destination,0);assert(b.s.nodes.edit(destination,b.s.route.identity(),mt::NodeCache::Edit::Ignore)==1);}
    auto original=packet(a,destination,303);b.receive(original,1000);assert(b.s.route.pending()==1);
    auto duplicate=original;duplicate[12]=uint8_t((2u<<5)|1u);duplicate[15]=0x77;b.receive(duplicate,1200);
    assert(b.s.route.pending()==1&&!b.s.route.next(duplicateDue - 1)&&b.s.route.next(duplicateDue));
  }
  pass("CLIENT_BASE-preserves-favorite-LoRa-duplicate-and-clamps-once-to-stock-late-window; nonfavorite-cancels");
  {
    Side a(0x11223344),late(0x22334455,11);auto p=packet(a,0xffffffffu,350,2);
    late.receive(p,1000);assert(late.s.route.pending()==1);auto duplicate=p;
    duplicate[12]=uint8_t((2u<<5)|1u);duplicate[15]=0x77;late.receive(duplicate,1200);
    assert(late.s.route.pending()==1&&!late.s.route.next(duplicateDue - 1)&&late.s.route.next(duplicateDue));
    Side opaque(0x33445566,11);p=packet(a,0xffffffffu,351,2);p[13]^=1;
    opaque.receive(p,1000);assert(opaque.s.route.pending()==1);duplicate=p;
    duplicate[12]=uint8_t((2u<<5)|1u);duplicate[15]=0x77;opaque.receive(duplicate,1200);
    assert(opaque.s.route.pending()==1&&!opaque.s.route.next(duplicateDue - 1)&&opaque.s.route.next(duplicateDue));
  }
  pass("ROUTER_LATE decoded-and-opaque duplicates preserve queued work and clamp only once");
  for (bool opaque : {false, true}) {
    Side a(0x11223344), late(0x22334455,11);
    auto original = packet(a,0xffffffffu,354,2);
    if (opaque) original[13] ^= 1;
    late.s.rx(original.data(),original.size(),-20,1000,0);
    auto duplicate = original;
    duplicate[12] = uint8_t((2u << 5) | 1u);
    duplicate[15] = 0x77;
    late.receive(duplicate,1200);
    constexpr uint32_t due = 1200 + stock_long_fast::weakDuplicateMs;
    assert(late.s.route.pending()==1 && !late.s.route.next(due-1) && late.s.route.next(due));
  }
  pass("ROUTER_LATE duplicate uses original negative SNR for decoded and opaque frames");
  {
    Side a(0x11223344),late(0x22334455,11);
    auto p=packet(a,0xffffffffu,352,2);late.receive(p,1000);late.receive(p,1200);
    assert(late.s.route.pending()==1&&!late.s.route.next(initialDue - 1)&&late.s.route.next(initialDue));
    Side opaque(0x33445566,11);p=packet(a,0xffffffffu,353,2);p[13]^=1;
    opaque.receive(p,1000);opaque.receive(p,1200);
    assert(opaque.s.route.pending()==1&&!opaque.s.route.next(initialDue - 1)&&opaque.s.route.next(initialDue));
  }
  pass("ROUTER_LATE repeated-originator decoded-and-opaque copies do not enter the late-duplicate window");
  auto forwardedHop=[](Side &side,std::vector<uint8_t> p,uint32_t now=1000) {
    side.receive(p,now);auto *queued=side.s.route.next(now+10000);assert(queued);return uint8_t(queued->bytes[12]&7u);
  };
  {
    Side a(0x11223344),first(0x22334455,11);auto p=packet(a,0xffffffffu,401,2);p[15]=0x44;
    favoriteNode(first,0x55667744,2);assert(forwardedHop(first,p)==1);
    Side second(0x22334455,11);p=packet(a,0xffffffffu,402,3);p[12]=uint8_t((3u<<5)|2u);p[15]=0x44;
    favoriteNode(second,0x55667744,2);assert(forwardedHop(second,p)==2);
    Side nonfavorite(0x22334455,11);p=packet(a,0xffffffffu,403,3);p[12]=uint8_t((3u<<5)|2u);p[15]=0x44;
    mt::NodeCache::Entry e{};e.identity(0x55667744,81,2);
    assert(mt::NodeCache::Entry::text(e.bytes+e.Name,40,"Router neighbor",16));
    assert(nonfavorite.s.nodes.accept(e,nonfavorite.s.route.identity(),-20,1)>=0);
    assert(forwardedHop(nonfavorite,p)==1);
    Side ambiguous(0x22334455,11);p=packet(a,0xffffffffu,404,3);p[12]=uint8_t((3u<<5)|2u);p[15]=0x44;
    favoriteNode(ambiguous,0x55667744,2);favoriteNode(ambiguous,0x11223344,12);
    assert(forwardedHop(ambiguous,p)==1);
    Side ignoredCollision(0x22334455,11);p=packet(a,0xffffffffu,405,3);p[12]=uint8_t((3u<<5)|2u);p[15]=0x44;
    favoriteNode(ignoredCollision,0x55667744,2);favoriteNode(ignoredCollision,0x66778844,12);
    assert(ignoredCollision.s.nodes.edit(0x66778844,ignoredCollision.s.route.identity(),mt::NodeCache::Edit::Ignore)==1);
    assert(forwardedHop(ignoredCollision,p)==2);
    Side placeholderCollision(0x22334455,11);p=packet(a,0xffffffffu,406,3);p[12]=uint8_t((3u<<5)|2u);p[15]=0x44;
    favoriteNode(placeholderCollision,0x55667744,2);placeholderFavorite(placeholderCollision,0x66778844);
    assert(forwardedHop(placeholderCollision,p)==1);
  }
  pass("CLIENT_BASE-and-ROUTER_LATE-first-hop-decrements-and-later-hop-preserves-only-unique-user-bearing-favorite-router-relay");
  for(unsigned mode:{0u,2u,3u,4u,5u}) {
    Side a(0x11223344),b(0x22334455,0,mode);auto p=packet(a,0x33445566);p[13]=0;
    b.receive(p);assert(b.s.route.pending()==unsigned(mode==0||mode==5));
    assert(!b.s.route.historyCount()&&!b.s.route.routeCount());
  }
  pass("opaque-PKI-transit-mode-gates-and-zero-trusted-history-side-effects");
  {
    Side a(0x11223344),b(0x22334455);auto p=packet(a,0x33445566,201,0);b.receive(p);assert(!b.s.route.pending());
    p=packet(a,0x33445566,202);b.receive(p);b.radio.result=-EBUSY;b.s.poll(10000,0);assert(!b.s.faulted());
    b.radio.result=0;b.s.poll(12000,0);assert(b.radio.frames.size()==1);b.complete(13000);assert(b.s.sent==1);
  }
  pass("hop-zero-exhaustion-and-native-busy-transit-retry-share-existing-completion-lifetime");
  {
    Side a(0x11223344),b(0x22334455);auto decoded=packet(a,0x33445566,601);b.receive(decoded);
    auto opaque=decoded;opaque[13]^=1;b.receive(opaque,1100);
    assert(b.s.route.pending()==1&&b.s.route.historyCount()==1&&!b.s.route.hasOpaqueHistory(a.s.route.identity(),601));
    opaque=packet(a,0x33445566,602);opaque[13]^=1;b.receive(opaque,1200);
    assert(b.s.route.pending()==2&&b.s.route.hasOpaqueHistory(a.s.route.identity(),602));
    decoded=packet(a,0x33445566,602);b.receive(decoded,1300);
    assert(b.s.route.pending()==2&&b.s.route.historyCount()==1&&!b.s.route.hasHistory(a.s.route.identity(),602));
  }
  pass("outer-ID-collision-keeps-decoded-opaque-queues-and-history-domains-separate-in-both-directions");
  for(unsigned mode:{2u,3u}) {
    Side a(0x11223344),b(0x22334455,0,mode);mt::NodeCache::Entry known;known.identity(a.s.route.identity(),81,1);
    assert(b.s.nodes.accept(known,b.s.route.identity(),1,0)>=0);auto frame=packet(a,0x33445566,603);frame[13]=0;
    b.receive(frame);assert(b.s.route.pending()==1&&b.s.route.historyCount()==0&&b.s.route.knownCount()==0);
  }
  pass("LOCAL_ONLY-and-KNOWN_ONLY-opaque-PKI-known-party-admission-never-learns-route-or-authentication");

  {
    Side a(0x11223344,0,0,"BatchLink");Radio radio;mt::Service service(radio,true,0,81,true,false);
    auto cfg=a.s.config;assert(cfg.setDeviceRole(2,5)&&service.apply(cfg));const uint32_t node=0x22334455;
    // Bind only the exact three boot route/discovery calls from main.cpp. This
    // keeps the test's production boot coupling explicit without requiring a
    // generated header outside the checked-in fixture set.
    service.route.configureContinuous(uint8_t(node),service.config.node().bytes[4]);
    service.route.configureIdentity(node);service.discovery.identity(node);
    assert(service.start(false,0)&&service.armEndpoint(true));
    const auto frame=packet(a,0x33445566,701);service.rx(frame.data(),frame.size(),3,1000,0);
    assert(service.codec.channelHash()==cfg.node().bytes[4]&&service.route.pending()==1);
  }
  pass("literal-main-boot-route-binding-retains-saved-named-channel-hash-and-actual-transit-admission");

}
