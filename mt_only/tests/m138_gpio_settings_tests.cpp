// SPDX-License-Identifier: GPL-3.0-only
#include "modules/GpioModuleJournal.h"
#include "modules/GpioModuleEdit.h"

#include <assert.h>
#include <errno.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace mt;
using namespace mt::modules;
using Bytes = std::vector<uint8_t>;

struct Backend final : SettingsBackend {
  enum class Fault { None, ReadError, BeforeWrite, CommittedError, TornError, TornSuccess,
                     ReadbackError, ReadbackShort, ReadbackDifferent };
  std::map<std::string, Bytes> records;
  std::vector<std::string> reads, writes;
  Fault fault = Fault::None;
  bool afterWrite = false;

  int read(const char *key, uint8_t *out, size_t capacity) override {
    reads.emplace_back(key);
    if (fault == Fault::ReadError && !afterWrite) return -EIO;
    if (fault == Fault::ReadbackError && afterWrite) return -EIO;
    auto found = records.find(key);
    if (found == records.end()) return -ENOENT;
    if (found->second.size() > capacity) return -E2BIG;
    const size_t size = found->second.size();
    if (size) memcpy(out, found->second.data(), size);
    if (afterWrite && fault == Fault::ReadbackDifferent && size) out[0] ^= 1;
    if (afterWrite && fault == Fault::ReadbackShort && size) return int(size - 1);
    return int(size);
  }
  int write(const char *key, const uint8_t *data, size_t size) override {
    writes.emplace_back(key); afterWrite = true;
    if (fault == Fault::BeforeWrite) return -EIO;
    records[key] = Bytes(data, data + size);
    if (fault == Fault::TornError || fault == Fault::TornSuccess)
      records[key].resize(size / 2);
    if (fault == Fault::CommittedError || fault == Fault::TornError) return -EIO;
    return 0;
  }
};

static unsigned groups = 0;
static void pass(const char *name) { ++groups; printf("PASS %s\n", name); }
static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  for (size_t i=0;i<n;++i) { crc^=p[i]; for(unsigned b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1))); }
  return ~crc;
}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void sealOuter(Bytes &record){put32(record.data()+GpioModuleJournal::RecordSize-4,crc32(record.data(),GpioModuleJournal::RecordSize-4));}
static void rawEnum(void *out,size_t size,uint32_t value){memcpy(out,&value,size);}
static void rawBool(bool &out,uint8_t value){memcpy(&out,&value,sizeof(value));}

static meshtastic_ModuleConfig remoteModule() {
  meshtastic_ModuleConfig out{};
  out.which_payload_variant=meshtastic_ModuleConfig_remote_hardware_tag;
  return out;
}
static meshtastic_ModuleConfig detectionModule() {
  meshtastic_ModuleConfig out{};
  out.which_payload_variant=meshtastic_ModuleConfig_detection_sensor_tag;
  return out;
}
static GpioModuleSettings makeSettings(const meshtastic_ModuleConfig &remote,
                                       const meshtastic_ModuleConfig &detection) {
  GpioModuleSettings out;
  assert(out.set(remote)); assert(out.set(detection)); assert(out.valid());
  return out;
}

static void stockDefaultsAndCanonicalRoundTrip() {
  GpioModuleSettings settings;
  assert(settings.valid());
  meshtastic_ModuleConfig out{};
  assert(settings.get(out,GpioModuleSettings::RemoteTag));
  assert(out.which_payload_variant==GpioModuleSettings::RemoteTag &&
         !out.payload_variant.remote_hardware.enabled &&
         !out.payload_variant.remote_hardware.allow_undefined_pin_access &&
         out.payload_variant.remote_hardware.available_pins_count==0);
  assert(settings.get(out,GpioModuleSettings::DetectionTag));
  const auto &d=out.payload_variant.detection_sensor;
  assert(!d.enabled && d.minimum_broadcast_secs==45 && !d.state_broadcast_secs &&
         !d.send_bell && d.name[0]==0 && d.monitor_pin==0 &&
         d.detection_trigger_type==meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH &&
         !d.use_pullup);
  const auto unchanged=settings;
  assert(!settings.get(out,1) && settings.same(unchanged));

  auto r=remoteModule();
  auto &remote=r.payload_variant.remote_hardware;
  remote.enabled=true;remote.allow_undefined_pin_access=true;remote.available_pins_count=4;
  const uint8_t pinNumbers[4]={0,1,31,63};
  const meshtastic_RemoteHardwarePinType types[4]={
    meshtastic_RemoteHardwarePinType_UNKNOWN,
    meshtastic_RemoteHardwarePinType_DIGITAL_READ,
    meshtastic_RemoteHardwarePinType_DIGITAL_WRITE,
    meshtastic_RemoteHardwarePinType_DIGITAL_READ};
  for(unsigned i=0;i<4;++i){remote.available_pins[i].gpio_pin=pinNumbers[i];remote.available_pins[i].type=types[i];}
  memcpy(remote.available_pins[0].name,"LED",4);
  memcpy(remote.available_pins[1].name,"Input",6);
  memcpy(remote.available_pins[2].name,"Relay",6);
  memcpy(remote.available_pins[3].name,"Bit63",6);

  auto det=detectionModule();auto &sensor=det.payload_variant.detection_sensor;
  sensor.enabled=true;sensor.minimum_broadcast_secs=UINT32_MAX;
  sensor.state_broadcast_secs=0x80000000u;sensor.send_bell=true;
  memcpy(sensor.name,"Motion",7);sensor.monitor_pin=63;
  sensor.detection_trigger_type=meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_EITHER_EDGE_ACTIVE_HIGH;
  sensor.use_pullup=true;
  auto full=makeSettings(r,det);
  uint8_t encoded[GpioModuleSettings::Size];
  assert(full.encode(encoded,sizeof(encoded)) && encoded[0]=='Z' && encoded[1]=='G' && encoded[2]=='M');
  GpioModuleSettings decoded;
  assert(GpioModuleSettings::decode(encoded,sizeof(encoded),decoded) && decoded.same(full));
  assert(decoded.get(out,GpioModuleSettings::RemoteTag));
  assert(out.payload_variant.remote_hardware.available_pins_count==4 &&
         out.payload_variant.remote_hardware.available_pins[3].gpio_pin==63 &&
         out.payload_variant.remote_hardware.available_pins[3].type==types[3]);
  assert(decoded.get(out,GpioModuleSettings::DetectionTag));
  assert(out.payload_variant.detection_sensor.minimum_broadcast_secs==UINT32_MAX &&
         out.payload_variant.detection_sensor.state_broadcast_secs==0x80000000u &&
         out.payload_variant.detection_sensor.monitor_pin==63);
  auto protobufZero=detectionModule();
  protobufZero.payload_variant.detection_sensor.minimum_broadcast_secs=0;
  protobufZero.payload_variant.detection_sensor.detection_trigger_type=
      meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_LOW;
  GpioModuleSettings zeroValues;assert(zeroValues.set(protobufZero));
  assert(zeroValues.encode(encoded,sizeof(encoded))&&GpioModuleSettings::decode(encoded,sizeof(encoded),decoded));
  assert(decoded.get(out,GpioModuleSettings::DetectionTag)&&
         out.payload_variant.detection_sensor.minimum_broadcast_secs==0&&
         out.payload_variant.detection_sensor.detection_trigger_type==
           meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_LOW);
  pass("stock defaults and both canonical protobuf submessages roundtrip with pins 0..63");
}

static void malformedConfigRejectsAtomically() {
  auto baseR=remoteModule();auto baseD=detectionModule();
  auto base=makeSettings(baseR,baseD);const auto original=base;
  auto r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=5;
  assert(!base.set(r)&&base.same(original));
  r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=1;
  r.payload_variant.remote_hardware.available_pins[0].gpio_pin=64;
  assert(!base.set(r)&&base.same(original));
  r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=1;
  rawEnum(&r.payload_variant.remote_hardware.available_pins[0].type,
          sizeof(r.payload_variant.remote_hardware.available_pins[0].type),UINT32_MAX);
  assert(!base.set(r)&&base.same(original));
  r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=1;
  memset(r.payload_variant.remote_hardware.available_pins[0].name,'x',
         sizeof(r.payload_variant.remote_hardware.available_pins[0].name));
  assert(!base.set(r)&&base.same(original));
  r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=0;
  rawBool(r.payload_variant.remote_hardware.enabled,2);
  assert(!base.set(r)&&base.same(original));
  r=remoteModule();r.payload_variant.remote_hardware.available_pins_count=1;
  r.payload_variant.remote_hardware.available_pins[0].gpio_pin=1;
  r.payload_variant.remote_hardware.available_pins[1].gpio_pin=2; // unused suffix must be zero
  assert(!base.set(r)&&base.same(original));

  auto d=detectionModule();d.payload_variant.detection_sensor.monitor_pin=64;
  assert(!base.set(d)&&base.same(original));
  d=detectionModule();rawEnum(&d.payload_variant.detection_sensor.detection_trigger_type,
                              sizeof(d.payload_variant.detection_sensor.detection_trigger_type),6);
  assert(!base.set(d)&&base.same(original));
  d=detectionModule();rawEnum(&d.payload_variant.detection_sensor.detection_trigger_type,
                              sizeof(d.payload_variant.detection_sensor.detection_trigger_type),UINT32_MAX);
  assert(!base.set(d)&&base.same(original));
  d=detectionModule();memset(d.payload_variant.detection_sensor.name,'m',sizeof(d.payload_variant.detection_sensor.name));
  assert(!base.set(d)&&base.same(original));
  d=detectionModule();rawBool(d.payload_variant.detection_sensor.send_bell,2);
  assert(!base.set(d)&&base.same(original));
  d=remoteModule();d.which_payload_variant=1;assert(!base.set(d)&&base.same(original));
  pass("invalid tags, count, pin bounds, enums, booleans, and string shapes reject atomically");
}

static void canonicalEncodingAndRecordIntegrity() {
  auto r=remoteModule();auto d=detectionModule();auto settings=makeSettings(r,d);
  uint8_t record[GpioModuleSettings::Size];assert(settings.encode(record,sizeof(record)));
  GpioModuleSettings out;assert(GpioModuleSettings::decode(record,sizeof(record),out));
  Bytes bad(record,record+sizeof(record));
  bad[GpioModuleSettings::RemoteOffset]=0x08; // noncanonical explicit false instead of omitted default
  bad[GpioModuleSettings::RemoteOffset+1]=0;
  bad[4]=2;
  put32(bad.data()+GpioModuleSettings::CrcOffset,crc32(bad.data(),GpioModuleSettings::CrcOffset));
  assert(!GpioModuleSettings::decode(bad.data(),bad.size(),out));
  bad.assign(record,record+sizeof(record));bad[GpioModuleSettings::DetectionOffset+GpioModuleSettings::DetectionCapacity-1]=1;
  put32(bad.data()+GpioModuleSettings::CrcOffset,crc32(bad.data(),GpioModuleSettings::CrcOffset));
  assert(!GpioModuleSettings::decode(bad.data(),bad.size(),out));
  bad.assign(record,record+sizeof(record));bad[0]^=1;assert(!GpioModuleSettings::decode(bad.data(),bad.size(),out));
  bad.assign(record,record+sizeof(record));bad.back()^=1;assert(!GpioModuleSettings::decode(bad.data(),bad.size(),out));
  assert(!GpioModuleSettings::decode(record,sizeof(record)-1,out));
  pass("fixed protobuf buffers reject noncanonical explicit defaults, nonzero padding, CRC, version, and length faults");
}

static void journalNamespaceLoadAndRepair() {
  Backend backend;
  backend.records["mtbase/state0"]={1,2,3};backend.records["mtmod/state0"]={4,5};
  backend.records["mtidentity/state0"]={6,7,8};const auto old=backend.records;
  GpioModuleJournal journal;
  assert(!journal.ready()&&journal.load(backend)==0&&journal.ready()&&
         journal.state()==GpioModuleJournal::State::Fresh&&journal.generation()==0&&journal.activeSlot()==-1);
  assert(backend.writes.empty()&&backend.reads==std::vector<std::string>({"mtgpio/state0","mtgpio/state1"}));
  const auto value=journal.config();assert(journal.save(backend,value)==0&&journal.state()==GpioModuleJournal::State::Loaded&&journal.generation()==1&&journal.activeSlot()==0);
  const auto &record=backend.records.at("mtgpio/state0");
  assert(record.size()==GpioModuleJournal::RecordSize&&record[0]=='Z'&&record[1]=='G'&&record[2]=='J'&&record[3]==1&&get32(record.data()+4)==1);
  assert(record[8]==GpioModuleSettings::Size&&record[9]==0&&get32(record.data()+record.size()-4)==crc32(record.data(),record.size()-4));
  assert(backend.records.at("mtbase/state0")==old.at("mtbase/state0")&&backend.records.at("mtmod/state0")==old.at("mtmod/state0")&&backend.records.at("mtidentity/state0")==old.at("mtidentity/state0"));
  const size_t writes=backend.writes.size();GpioModuleJournal reload;
  assert(reload.load(backend)==0&&reload.state()==GpioModuleJournal::State::Loaded&&reload.config().same(value)&&backend.writes.size()==writes);
  assert(reload.save(backend,value)==0&&backend.writes.size()==writes);

  Backend recovered=backend;recovered.records["mtgpio/state1"]={1,2,3};
  recovered.reads.clear();recovered.writes.clear();
  GpioModuleJournal repair;assert(repair.load(recovered)==0&&repair.state()==GpioModuleJournal::State::Recovered&&repair.ready());
  assert(recovered.writes.empty());assert(repair.save(recovered,repair.config())==0&&repair.state()==GpioModuleJournal::State::Loaded&&repair.generation()==2&&repair.activeSlot()==1);
  pass("journal load is read-only, owns only mtgpio slots, verifies persistence, and equal-value repair is explicit");
}

static void journalAmbiguityCorruptionAndFailures() {
  auto aSettings=GpioModuleSettings{};auto bSettings=aSettings;
  auto d=detectionModule();d.payload_variant.detection_sensor.enabled=true;assert(bSettings.set(d));
  Backend seed;GpioModuleJournal w;assert(w.load(seed)==0&&w.save(seed,aSettings)==0&&w.save(seed,bSettings)==0);
  auto same=seed;put32(same.records["mtgpio/state1"].data()+4,1);sealOuter(same.records["mtgpio/state1"]);
  GpioModuleJournal s;assert(s.load(same)==-EINVAL&&s.state()==GpioModuleJournal::State::Invalid);
  auto half=seed;put32(half.records["mtgpio/state1"].data()+4,0x80000001u);sealOuter(half.records["mtgpio/state1"]);
  GpioModuleJournal h;assert(h.load(half)==-EINVAL&&h.state()==GpioModuleJournal::State::Invalid);
  auto conflict=seed;put32(conflict.records["mtgpio/state1"].data()+4,1);memcpy(conflict.records["mtgpio/state1"].data()+GpioModuleJournal::HeaderSize,
      conflict.records["mtgpio/state0"].data()+GpioModuleJournal::HeaderSize,GpioModuleSettings::Size);sealOuter(conflict.records["mtgpio/state1"]);
  GpioModuleJournal c;assert(c.load(conflict)==0&&c.state()==GpioModuleJournal::State::Loaded); // same generation, same value is coherent

  Backend wrap;GpioModuleJournal wrapWriter;assert(wrapWriter.load(wrap)==0);
  assert(wrapWriter.save(wrap,aSettings)==0&&wrapWriter.save(wrap,bSettings)==0);
  put32(wrap.records["mtgpio/state0"].data()+4,UINT32_MAX);sealOuter(wrap.records["mtgpio/state0"]);
  put32(wrap.records["mtgpio/state1"].data()+4,0);sealOuter(wrap.records["mtgpio/state1"]);
  GpioModuleJournal wrapped;assert(wrapped.load(wrap)==0&&wrapped.activeSlot()==1&&wrapped.generation()==0&&wrapped.config().same(bSettings));

  Backend corrupt;corrupt.records["mtgpio/state0"]={0};GpioModuleJournal bad;
  assert(bad.load(corrupt)==-EINVAL&&bad.state()==GpioModuleJournal::State::Invalid);
  Backend io;io.fault=Backend::Fault::ReadError;
  GpioModuleJournal ioe;assert(ioe.load(io)==-EIO&&ioe.state()==GpioModuleJournal::State::IoError);

  for(Backend::Fault fault:{Backend::Fault::BeforeWrite,Backend::Fault::CommittedError,Backend::Fault::TornError,
      Backend::Fault::TornSuccess,Backend::Fault::ReadbackError,Backend::Fault::ReadbackShort,Backend::Fault::ReadbackDifferent}){
    Backend backend;GpioModuleJournal journal;assert(journal.load(backend)==0);backend.fault=fault;
    assert(journal.save(backend,bSettings)!=0&&journal.state()==GpioModuleJournal::State::IoError);
    const size_t attempts=backend.writes.size();assert(journal.save(backend,bSettings)==-EACCES&&backend.writes.size()==attempts);
  }
  pass("same-generation conflicts/half-range fail closed, corruption and I/O fail closed, uncertain writes poison");
}

int main(){
  stockDefaultsAndCanonicalRoundTrip();
  malformedConfigRejectsAtomically();
  canonicalEncodingAndRecordIntegrity();
  journalNamespaceLoadAndRepair();
  journalAmbiguityCorruptionAndFailures();
  printf("RESULT PASS groups=%u\n",groups);
}
