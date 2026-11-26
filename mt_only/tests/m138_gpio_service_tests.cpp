// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Codec/Companion/GPIO service path; only GPIO backend and
// settings persistence are memory fixtures. No GPIO driver, target, or RF.
#include "endpoint_fixture.h"
#include "known_channel_fixture.h"
#include "modules/GpioModuleEdit.h"
#include "modules/GpioModuleRuntime.h"
#include "modules/GpioModuleService.h"

#include <cassert>
#include <map>
#include <string>

using namespace mt::modules;

struct GpioRadio final : mt::Radio {
  int state=0;unsigned sends=0;uint32_t air=100;bool done=false;
  uint8_t *pending=nullptr;std::vector<std::vector<uint8_t>> frames;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int stop()override{state=0;pending=nullptr;return 0;}
  int restore()override{state=1;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return air;}
  int send(uint8_t *p,size_t n)override{++sends;frames.emplace_back(p,p+n);pending=p;done=false;return 0;}
  bool completion(int &result)override{result=0;return done;}
};
struct GpioHw final : GpioModuleBackend {
  GpioCapabilities caps{(UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<3),
                        (UINT64_C(1)<<1)|(UINT64_C(1)<<3),
                        UINT64_C(1)<<2,
                        (UINT64_C(1)<<1)|(UINT64_C(1)<<3)};
  uint64_t value=0;unsigned configures=0,reads=0,writes=0;
  uint64_t lastRead=0,lastWrite=0,lastWritten=0;
  int configureRc=0,readRc=0,writeRc=0;
  GpioCapabilities capabilities()const override{return caps;}
  int configureInput(uint32_t pin,bool)override{++configures;assert(pin<64);return configureRc;}
  int read(uint64_t mask,bool,uint64_t &out)override{++reads;lastRead=mask;out=value;return readRc;}
  int write(uint64_t mask,uint64_t bits)override{++writes;lastWrite=mask;lastWritten=bits;return writeRc;}
};
struct MemSettings final : mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> rows;unsigned writes=0;bool fail=false;
  int read(const char *key,uint8_t *out,size_t cap)override{
    auto it=rows.find(key);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *bytes,size_t size)override{
    ++writes;if(fail)return -EIO;rows[key]=std::vector<uint8_t>(bytes,bytes+size);return 0;
  }
};
struct GpioHost final : mt::Companion::Host {
  mt::Service &service;MemSettings &backend;GpioModuleJournal &journal;
  GpioModuleEdit &edit;GpioModuleRuntime &runtime;GpioModuleService &gpio;
  std::vector<meshtastic_FromRadio> out;bool editingNow=false;uint32_t now=0;
  unsigned saveCalls=0;
  GpioHost(mt::Service &s,MemSettings &b,GpioModuleJournal &j,GpioModuleEdit &e,
           GpioModuleRuntime &r,GpioModuleService &g):service(s),backend(b),journal(j),edit(e),runtime(r),gpio(g){}
  bool ready()const override{return journal.ready();}
  bool send(const uint8_t *bytes,size_t size)override{
    assert(size>=4&&size_t(bytes[2]*256+bytes[3])==size-4);
    meshtastic_FromRadio f{};auto stream=pb_istream_from_buffer(bytes+4,size-4);
    assert(pb_decode(&stream,meshtastic_FromRadio_fields,&f)&&stream.bytes_left==0);
    out.push_back(f);return true;
  }
  int save(const mt::Config &)override{return -ENOTSUP;}
  uint32_t supportedModules()const override{
    return ready()&&runtime.available()?
      meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG|meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG:0;
  }
  bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag)const override{
    return ready()&&edit.config().get(out,tag);
  }
  int saveModuleConfig(const meshtastic_ModuleConfig &config,uint32_t t)override{
    ++saveCalls;
    GpioModuleSettings candidate=edit.config();
    if(!candidate.set(config)||!runtime.validateConfig(candidate))return -ENOTSUP;
    const int rc=edit.save(config,t,editingNow);
    if(!rc) {
      const bool live=edit.trusted()&&!editingNow&&service.receiving()&&!service.faulted();
      gpio.configure(edit.config(),live, journal.generation()+uint32_t(editingNow));
    }
    return rc;
  }
  int beginEdit(uint32_t t)override{(void)t;if(editingNow)return -EBUSY;editingNow=true;return 0;}
  int commitEdit(uint32_t t)override{
    const int rc=edit.commit(t);if(!rc){editingNow=false;gpio.configure(edit.config(),
      edit.trusted()&&service.receiving()&&!service.faulted(),journal.generation());}return rc;
  }
  bool editing()const override{return editingNow;}
};

static unsigned groups=0;
static void pass(const char *name){++groups;printf("PASS %s\n",name);}
static meshtastic_ModuleConfig remoteConfig(bool enabled=true) {
  meshtastic_ModuleConfig c{};c.which_payload_variant=GpioModuleSettings::RemoteTag;
  auto &r=c.payload_variant.remote_hardware;r.enabled=enabled;r.allow_undefined_pin_access=false;
  if(enabled){r.available_pins_count=2;r.available_pins[0].gpio_pin=1;
    r.available_pins[0].type=meshtastic_RemoteHardwarePinType_DIGITAL_READ;
    r.available_pins[1].gpio_pin=2;r.available_pins[1].type=meshtastic_RemoteHardwarePinType_DIGITAL_WRITE;}
  return c;
}
static meshtastic_ModuleConfig detectionConfig(bool enabled,const char *name="door") {
  meshtastic_ModuleConfig c{};c.which_payload_variant=GpioModuleSettings::DetectionTag;
  auto &d=c.payload_variant.detection_sensor;d.enabled=enabled;d.minimum_broadcast_secs=1;
  d.send_bell=true;
  d.monitor_pin=3;d.detection_trigger_type=meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH;
  if(name)snprintf(d.name,sizeof(d.name),"%s",name);
  return c;
}
struct LocalCapture {unsigned count=0;uint32_t id=0;unsigned channel=0;meshtastic_Data data{};};
static void localPacket(void *ctx,const uint8_t *bytes,size_t n,uint32_t id,unsigned channel) {
  auto &capture=*static_cast<LocalCapture *>(ctx);assert(n<=64);
  auto stream=pb_istream_from_buffer(bytes,n);assert(pb_decode(&stream,meshtastic_Data_fields,&capture.data));
  ++capture.count;capture.id=id;capture.channel=channel;
}
static void receiveThunk(void *ctx,const uint8_t *frame,size_t size,const bench::RouteMetadata &meta,
                         unsigned channel,uint32_t now,uint32_t random) {
  static_cast<GpioModuleService *>(ctx)->receive(frame,size,meta,channel,now,random);
}

struct GpioFixture {
  mt::test::KnownChannelFixtureClock channelClock;
  GpioRadio radio;mt::Service service;MemSettings backend;GpioModuleJournal journal;
  GpioHw hw;GpioModuleRuntime runtime;LocalCapture local;
  GpioModuleService gpio;GpioModuleEdit edit;GpioHost host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  unsigned id=1000;
  GpioFixture():service(radio,true,OURS,81,true,false),runtime(hw),
    gpio(service,runtime,&localPacket,&local),edit(service,journal,backend),
    host(service,backend,journal,edit,runtime,gpio),companion(service,host,81,workspace,0) {
    channelClock.bind(service);
    mt::Config cfg=service.config;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(journal.load(backend)==0&&edit.load());assert(service.apply(cfg));
    assert(!service.receiving());
    assert(service.bindGpioFrameSink(receiveThunk,&gpio));
    assert(service.start(false,0)&&service.armEndpoint(true));
    assert(gpio.configure(edit.config(),true,journal.generation()));
    handshake();
  }
  void handshake(){
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;
    uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));companion.packet(bytes,stream.bytes_written,1,mt::Companion::WorkspaceProof{true});
    host.out.clear();
  }
  void input(const meshtastic_ToRadio &r,uint32_t now=10) {
    uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_ToRadio_fields,&r));companion.packet(bytes,stream.bytes_written,now,mt::Companion::WorkspaceProof{true});
  }
  void admin(const meshtastic_AdminMessage &a,uint32_t now=10) {
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.to=OURS;p.id=++id;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=stream.bytes_written;
    input(r,now);
  }
  int error()const{
    assert(!host.out.empty());const auto &p=host.out.back().packet;
    assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);
    meshtastic_Routing r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
    assert(pb_decode(&stream,meshtastic_Routing_fields,&r));return r.error_reason;
  }
  meshtastic_AdminMessage reply()const{
    assert(!host.out.empty());const auto &p=host.out.back().packet;
    assert(p.decoded.portnum==meshtastic_PortNum_ADMIN_APP);
    meshtastic_AdminMessage a{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
    assert(pb_decode(&stream,meshtastic_AdminMessage_fields,&a));return a;
  }
  void setChannels() {
    service.stop();auto cfg=service.config;
    mt::ChannelRecord c1,c2,c3;
    assert(cfg.setPrimaryName("gPiO"));
    assert(c1.set(2,"gPiO",nullptr,0));assert(c2.set(2,"GPIO",nullptr,0));
    assert(c3.set(2,"other",nullptr,0));
    assert(cfg.setSecondaryChannel(1,c1)&&cfg.setSecondaryChannel(2,c2)&&cfg.setSecondaryChannel(3,c3));
    assert(service.apply(cfg)&&service.start(false,100)&&service.armEndpoint(true));
    assert(GpioModuleChannels::gpio(service.config,0)&&GpioModuleChannels::gpio(service.config,1)&&
           GpioModuleChannels::gpio(service.config,2)&&!GpioModuleChannels::gpio(service.config,3));
    assert(gpio.configure(edit.config(),edit.trusted(),journal.generation()+1));
  }
  std::vector<uint8_t> makeFrame(const uint8_t *data,size_t n,unsigned slot,uint32_t from,uint32_t to,uint32_t packetId) {
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(slot,key));
    bench::MtCodec codec;assert(codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    uint8_t frame[255]{};size_t size=0;
    assert(codec.encodeZeroHopData(from,to,packetId,data,n,frame,sizeof(frame),size));
    frame[12]=uint8_t((2u<<5)|2u);
    return {frame,frame+size};
  }
  std::vector<uint8_t> makeRequest(unsigned slot,uint32_t from,uint32_t to,uint32_t packetId,
                                   meshtastic_HardwareMessage_Type type,uint64_t mask,uint64_t value=0,
                                   bool hasBitfield=true,uint32_t bitfield=2) {
    meshtastic_HardwareMessage h=meshtastic_HardwareMessage_init_default;
    h.type=type;h.gpio_mask=mask;h.gpio_value=value;
    uint8_t payload[meshtastic_HardwareMessage_size]{};auto hp=pb_ostream_from_buffer(payload,sizeof(payload));
    assert(pb_encode(&hp,meshtastic_HardwareMessage_fields,&h));
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
    d.payload.size=hp.bytes_written;memcpy(d.payload.bytes,payload,hp.bytes_written);
    d.has_bitfield=hasBitfield;d.bitfield=bitfield;
    uint8_t data[239]{};auto out=pb_ostream_from_buffer(data,sizeof(data));
    assert(pb_encode(&out,meshtastic_Data_fields,&d));
    return makeFrame(data,out.bytes_written,slot,from,to,packetId);
  }
  std::vector<uint8_t> makeRawRequest(unsigned slot,uint32_t from,uint32_t to,uint32_t packetId,
                                     const uint8_t *hardware,size_t hardwareSize,
                                     bool hasBitfield=true,uint32_t bitfield=2) {
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
    d.payload.size=pb_size_t(hardwareSize);memcpy(d.payload.bytes,hardware,hardwareSize);
    d.has_bitfield=hasBitfield;d.bitfield=bitfield;
    uint8_t data[239]{};auto out=pb_ostream_from_buffer(data,sizeof(data));
    assert(pb_encode(&out,meshtastic_Data_fields,&d));
    return makeFrame(data,out.bytes_written,slot,from,to,packetId);
  }
  void inject(const std::vector<uint8_t> &frame,uint32_t now=500,bool radioReceive=true) {
    service.rx(frame.data(),frame.size(),-70,now,now+1,radioReceive);
  }
  void serviceTx(uint32_t now=900) {service.poll(now,0);}
};

static void companionAdminAndEdit() {
  GpioFixture f;
  assert(f.host.supportedModules()&
    meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG);
  meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;
  get.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG;
  f.admin(get);assert(f.reply().which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag);
  assert(f.reply().get_module_config_response.which_payload_variant==GpioModuleSettings::RemoteTag);
  const unsigned before=f.backend.writes;
  meshtastic_AdminMessage begin{};begin.which_payload_variant=meshtastic_AdminMessage_begin_edit_settings_tag;
  begin.begin_edit_settings=true;f.admin(begin,20);assert(f.error()==meshtastic_Routing_Error_NONE&&f.host.editingNow);
  meshtastic_AdminMessage set{};set.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;
  set.set_module_config=remoteConfig(true);f.admin(set,30);
  assert(f.error()==meshtastic_Routing_Error_NONE&&f.edit.dirty()&&f.backend.writes==before);
  meshtastic_AdminMessage commit{};commit.which_payload_variant=meshtastic_AdminMessage_commit_edit_settings_tag;
  commit.commit_edit_settings=true;f.admin(commit,40);
  assert(f.error()==meshtastic_Routing_Error_NONE&&!f.edit.dirty()&&f.edit.trusted());
  assert(f.backend.writes==before+1&&f.service.receiving()&&f.service.endpointArmed());
  const auto persisted=f.edit.config();
  f.admin(set,50);assert(f.error()==meshtastic_Routing_Error_NONE&&f.backend.writes==before+1);
  GpioModuleJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().same(persisted));
  get.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG;
  f.admin(get,60);assert(f.reply().get_module_config_response.payload_variant.remote_hardware.enabled);
  pass("Companion GPIO get/set uses real editor, one commit persists, same-value setter is no-op");
}

static void serviceRequestIntegration() {
  GpioFixture f;f.setChannels();
  meshtastic_ModuleConfig remote=remoteConfig(true);
  GpioModuleSettings settings;assert(settings.set(remote)&&f.runtime.validateConfig(settings));
  assert(f.gpio.configure(settings,true,f.journal.generation()+2));

  auto read=f.makeRequest(1,PEER,OURS,2001,meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
  f.hw.value=UINT64_C(1)<<1;f.inject(read);assert(f.hw.reads==1&&f.hw.lastRead==(UINT64_C(1)<<1));
  f.inject(read);assert(f.hw.reads==1); // One backend effect for duplicate wire packet.
  f.serviceTx(900);assert(f.radio.sends==1);
  bench::RouteMetadata meta;const auto &wire=f.radio.frames.back();mt::PrimaryChannel::Selection channel{};
  assert(f.service.config.channelSelection(1,channel));bench::MtCodec decode;
  assert(decode.setKey(channel.hash,channel.bytes,channel.length));mt::PrimaryChannel::clear(channel);
  assert(decode.decode(wire.data(),wire.size(),meta)&&meta.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP&&meta.request_id==2001);
  meshtastic_HardwareMessage reply{};assert(GpioModuleWire::hardware(meta.payload,meta.payloadLength,reply));
  assert(reply.type==meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY&&reply.gpio_mask==(UINT64_C(1)<<1)&&reply.gpio_value==(UINT64_C(1)<<1));

  auto read2=f.makeRequest(2,PEER+1,OURS,2002,meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
  f.inject(read2,1000);assert(f.hw.reads==2); // Secondary case-insensitive alias shares the same Public key context.
  auto write=f.makeRequest(1,PEER,OURS,2003,meshtastic_HardwareMessage_Type_WRITE_GPIOS,
                           UINT64_C(1)<<2,UINT64_C(1)<<2);
  f.inject(write,1100);f.inject(write,1101);assert(f.hw.writes==1&&f.hw.lastWrite==(UINT64_C(1)<<2)&&f.hw.lastWritten==(UINT64_C(1)<<2));
  pass("real Service decode/channel gate reaches GPIO runtime; public READ reply and duplicate WRITE are bounded");
}

static void noIoAndMalformedIngress() {
  GpioFixture f;f.setChannels();auto request=f.makeRequest(1,PEER,OURS,3001,
    meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
  const unsigned reads=f.hw.reads,writes=f.hw.writes;
  assert(f.gpio.configure(f.edit.config(),false,f.journal.generation()+3));
  f.inject(request,1200);assert(f.hw.reads==reads&&f.hw.writes==writes);
  auto settings=GpioModuleSettings{};assert(settings.set(remoteConfig(true)));
  assert(f.gpio.configure(settings,true,f.journal.generation()+4));
  auto nonlocal=f.makeRequest(1,PEER,PEER+4,3002,meshtastic_HardwareMessage_Type_WRITE_GPIOS,
                              UINT64_C(1)<<2,UINT64_C(1)<<2);
  f.inject(nonlocal,1300);assert(f.hw.writes==writes);
  auto own=f.makeRequest(1,OURS,OURS,3003,meshtastic_HardwareMessage_Type_WRITE_GPIOS,
                         UINT64_C(1)<<2,UINT64_C(1)<<2);
  f.inject(own,1400);assert(f.hw.writes==writes);
  f.inject(request,1500,false);assert(f.hw.reads==reads&&f.hw.writes==writes);
  auto partial=f.makeRequest(1,PEER+6,OURS,3005,meshtastic_HardwareMessage_Type_READ_GPIOS,
                             (UINT64_C(1)<<1)|(UINT64_C(1)<<3));
  const unsigned beforeConfigure=f.hw.configures;f.inject(partial,1700);
  assert(f.hw.reads==reads&&f.hw.configures==beforeConfigure);

  // Append unknown/duplicate Data fields, and unknown inner HardwareMessage
  // fields, then encrypt them through the same channel codec. Metadata parsing
  // can succeed, but canonical reconstruction must refuse before any GPIO I/O.
  uint8_t encoded[239]{};auto requestData=[&](){
    meshtastic_HardwareMessage h=meshtastic_HardwareMessage_init_default;
    h.type=meshtastic_HardwareMessage_Type_WRITE_GPIOS;h.gpio_mask=UINT64_C(1)<<2;h.gpio_value=UINT64_C(1)<<2;
    uint8_t hw[meshtastic_HardwareMessage_size]{};auto hs=pb_ostream_from_buffer(hw,sizeof(hw));
    assert(pb_encode(&hs,meshtastic_HardwareMessage_fields,&h));
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;d.has_bitfield=true;d.bitfield=2;
    d.payload.size=hs.bytes_written;memcpy(d.payload.bytes,hw,hs.bytes_written);
    auto ds=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&ds,meshtastic_Data_fields,&d));return size_t(ds.bytes_written);
  };
  size_t n=requestData();encoded[n++]=0xa0;encoded[n++]=0x06;encoded[n++]=1;
  auto unknown=f.makeFrame(encoded,n,1,PEER+7,OURS,3006);f.inject(unknown,1800);
  assert(f.hw.writes==writes);
  n=requestData(); // Duplicate the canonical payload field exactly.
  meshtastic_HardwareMessage hw{};hw.type=meshtastic_HardwareMessage_Type_WRITE_GPIOS;
  hw.gpio_mask=UINT64_C(1)<<2;hw.gpio_value=UINT64_C(1)<<2;
  uint8_t payload[meshtastic_HardwareMessage_size]{};auto hp=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&hp,meshtastic_HardwareMessage_fields,&hw));
  encoded[n++]=0x12;encoded[n++]=uint8_t(hp.bytes_written);
  memcpy(encoded+n,payload,hp.bytes_written);n+=hp.bytes_written;
  auto duplicate=f.makeFrame(encoded,n,1,PEER+8,OURS,3007);f.inject(duplicate,1900);
  assert(f.hw.writes==writes);
  uint8_t unknownHardware[meshtastic_HardwareMessage_size+3]{};
  memcpy(unknownHardware,payload,hp.bytes_written);
  unknownHardware[hp.bytes_written++]=0xa0;unknownHardware[hp.bytes_written++]=0x06;
  unknownHardware[hp.bytes_written++]=1;
  auto badHardware=f.makeRawRequest(1,PEER+9,OURS,3008,unknownHardware,hp.bytes_written);
  f.inject(badHardware,2000);assert(f.hw.writes==writes);
  GpioFixture unbound;GpioModuleSettings unboundSettings;assert(unboundSettings.set(remoteConfig(true)));
  assert(unbound.gpio.configure(unboundSettings,true,unbound.journal.generation()+44));
  auto noNamedChannel=unbound.makeRequest(0,PEER+10,OURS,3009,
    meshtastic_HardwareMessage_Type_WRITE_GPIOS,UINT64_C(1)<<2,UINT64_C(1)<<2);
  unbound.inject(noNamedChannel,2100);assert(unbound.hw.writes==0&&unbound.hw.reads==0);
  pass("unavailable readiness, wrong destination/source/path/channel, partial mask and noncanonical Data fail before IO");
}

static void detectionPortAndPrimaryChannelGate() {
  GpioFixture f;GpioModuleSettings settings;assert(settings.set(detectionConfig(true,"d\xc3\xb6or")));
  assert(f.runtime.validateConfig(settings));
  assert(f.gpio.configure(settings,true,f.journal.generation()+20));
  assert(GpioModuleChannels::stockDefaultPrimary(f.service.config));
  f.gpio.poll(0,100);f.hw.value=0;f.gpio.poll(100,101);
  f.hw.value=UINT64_C(1)<<3;f.gpio.poll(1000,102);
  assert(!f.local.count&&f.service.endpointControls()==0);
  f.service.stop();auto renamed=f.service.config;assert(renamed.setPrimaryName("Renamed"));
  assert(f.service.apply(renamed)&&f.service.start(false,1200)&&f.service.armEndpoint(true));
  assert(!GpioModuleChannels::stockDefaultPrimary(f.service.config));
  assert(f.gpio.configure(settings,true,f.journal.generation()+21));
  f.gpio.poll(1200,103);f.hw.value=0;f.gpio.poll(1300,104);
  f.hw.value=UINT64_C(1)<<3;f.gpio.poll(2300,105);
  assert(f.local.count==1&&f.local.id&&f.local.channel==0);
  assert(f.local.data.portnum==meshtastic_PortNum_DETECTION_SENSOR_APP);
  assert(f.local.data.payload.size==size_t(5+9+1));
  assert(!memcmp(f.local.data.payload.bytes,"d\xc3\xb6or detected",14));
  assert(f.local.data.payload.bytes[14]==0x07);
  f.serviceTx(2600);assert(f.radio.sends==1);
  bench::RouteMetadata meta;const auto &wire=f.radio.frames.back();
  assert(f.service.codec.decode(wire.data(),wire.size(),meta)&&meta.portnum==meshtastic_PortNum_DETECTION_SENSOR_APP);
  assert(meta.payloadLength==15&&!memcmp(meta.payload,"d\xc3\xb6or detected",14)&&meta.payload[14]==0x07);
  pass("port10 event is suppressed only on stock default primary; renamed public key allows UTF-8 event");
}

static void serviceEndpointBackpressure() {
  GpioFixture f;f.setChannels();GpioModuleSettings settings;
  assert(settings.set(remoteConfig(true))&&f.gpio.configure(settings,true,f.journal.generation()+80));
  for(unsigned i=0;i<4;++i) {
    auto occupant=f.makeRequest(1,OURS,PEER+80+i,4000+i,
      meshtastic_HardwareMessage_Type_WRITE_GPIOS,UINT64_C(1)<<2,UINT64_C(1)<<2);
    assert(!f.service.submitMessage(0,occupant.data(),occupant.size(),false,200+i));
  }
  assert(f.service.endpointFree()==0);
  auto request=f.makeRequest(1,PEER+20,OURS,4100,
    meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
  f.inject(request,500);assert(f.hw.reads==1&&f.runtime.pendingOutput());
  auto second=f.makeRequest(1,PEER+21,OURS,4101,
    meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
  f.inject(second,510);assert(f.hw.reads==1&&f.runtime.pendingOutput());
  f.service.disconnect(0);f.gpio.poll(1000,5000);
  assert(!f.runtime.pendingOutput()&&f.service.endpointFree()==3);
  pass("full real Service endpoint retains one GPIO reply and emits it after bounded queue pressure clears");
}

int main() {
  companionAdminAndEdit();
  serviceRequestIntegration();
  noIoAndMalformedIngress();
  detectionPortAndPrimaryChannelGate();
  serviceEndpointBackpressure();
  printf("PASS groups=%u\n",groups);
  return 0;
}
