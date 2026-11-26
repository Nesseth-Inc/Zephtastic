#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/GpioModuleRuntime.h"

using namespace mt::modules;
static unsigned groups=0;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
#define PASS(x) do { ++groups; printf("PASS %s\n",x); } while(0)

struct Mock final : GpioModuleBackend {
  GpioCapabilities caps{};
  unsigned inputs=0,reads=0,writes=0;
  uint64_t lastRead=0,lastWrite=0,lastValue=0;
  int readRc=0,writeRc=0,inputRc=0;
  GpioCapabilities capabilities() const override { return caps; }
  int configureInput(uint32_t pin,bool) override { ++inputs; CHECK(pin<64); return inputRc; }
  int read(uint64_t mask,bool,uint64_t &value) override { ++reads; lastRead=mask; value=lastValue; return readRc; }
  int write(uint64_t mask,uint64_t value) override { ++writes; lastWrite=mask;lastValue=value;return writeRc; }
};
struct Sink { bool accept=true; unsigned calls=0; GpioModuleRuntime::Outbound last{}; };
static bool sendPacket(void *p,const GpioModuleRuntime::Outbound &o) {
  auto &s=*static_cast<Sink*>(p);++s.calls;s.last=o;return s.accept;
}
static GpioModuleSettings withRemote(bool enabled,bool allow=false) {
  GpioModuleSettings s;
  meshtastic_ModuleConfig m{};m.which_payload_variant=GpioModuleSettings::RemoteTag;
  auto &r=m.payload_variant.remote_hardware;
  r=meshtastic_ModuleConfig_RemoteHardwareConfig_init_zero;r.enabled=enabled;
  r.allow_undefined_pin_access=allow;
  if(enabled&&!allow){r.available_pins_count=2;r.available_pins[0].gpio_pin=1;
    r.available_pins[0].type=meshtastic_RemoteHardwarePinType_DIGITAL_READ;
    r.available_pins[1].gpio_pin=2;
    r.available_pins[1].type=meshtastic_RemoteHardwarePinType_DIGITAL_WRITE;}
  CHECK(s.set(m));return s;
}
static GpioModuleSettings withDetection(uint32_t pin) {
  GpioModuleSettings s;
  meshtastic_ModuleConfig m{};m.which_payload_variant=GpioModuleSettings::DetectionTag;
  auto &d=m.payload_variant.detection_sensor;
  d=meshtastic_ModuleConfig_DetectionSensorConfig_init_zero;d.enabled=true;
  d.monitor_pin=pin;d.minimum_broadcast_secs=1;
  d.detection_trigger_type=meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH;
  strcpy(d.name,"door");CHECK(s.set(m));return s;
}
static GpioModuleRuntime::RequestContext req(uint32_t from,uint32_t id,uint8_t ch=2) {
  GpioModuleRuntime::RequestContext c{};c.from=from;c.packetId=id;c.channel=ch;
  c.replyHopLimit=3;c.gpioChannel=true;c.wantAck=true;return c;
}
static meshtastic_HardwareMessage message(meshtastic_HardwareMessage_Type type,uint64_t mask,uint64_t value=0) {
  meshtastic_HardwareMessage m=meshtastic_HardwareMessage_init_default;
  m.type=type;m.gpio_mask=mask;m.gpio_value=value;return m;
}
int main() {
  {
    Mock hw;GpioModuleRuntime rt(hw);Sink sink;
    GpioModuleSettings disabled;
    CHECK(rt.validateConfig(disabled));
    CHECK(rt.configure(disabled,false,false,0));
    CHECK(!rt.available());
    auto unsupported=withRemote(true);
    CHECK(!rt.validateConfig(unsupported));
    CHECK(rt.configure(unsupported,true,true,1u<<2));
    CHECK(!rt.available());
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,2),req(10,1),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Unavailable);
    rt.poll(1,false,4,sendPacket,&sink);
    CHECK(hw.inputs==0&&hw.reads==0&&hw.writes==0);
    PASS("zero-export-unavailable-no-io");
  }
  {
    Mock hw;hw.caps={ (UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<63),
      (UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<63),
      (UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<63),
      (UINT64_C(1)<<1)|(UINT64_C(1)<<2)|(UINT64_C(1)<<63) };
    GpioModuleRuntime rt(hw);Sink sink;
    CHECK(rt.configure(withRemote(true),true,true,1u<<2));
    hw.lastValue=UINT64_C(1)<<1;
    auto c=req(10,101);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1),c,sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Applied);
    CHECK(hw.inputs==1&&hw.reads==1&&sink.calls==1);
    CHECK(sink.last.to==10&&sink.last.requestId==101&&sink.last.channel==2&&sink.last.wantAck);
    CHECK(sink.last.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1),c,sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Duplicate);
    CHECK(hw.reads==1&&sink.calls==1);
    CHECK(rt.configure(withRemote(true),true,true,1u<<2,1));
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1),c,sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Applied);
    CHECK(hw.reads==2&&sink.calls==2);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_WRITE_GPIOS,UINT64_C(1)<<2,UINT64_C(1)<<2),req(11,102),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Applied);
    CHECK(hw.writes==1&&hw.lastWrite==(UINT64_C(1)<<2));
    PASS("authorized-effects-read-reply-and-duplicate");
  }
  {
    Mock hw;hw.caps={7,7,7,3};GpioModuleRuntime rt(hw);Sink sink;
    CHECK(rt.configure(withRemote(true),true,true,1u<<2));
    uint64_t partial=(UINT64_C(1)<<1)|(UINT64_C(1)<<2);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,partial),req(10,1),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Denied);
    CHECK(hw.inputs==0&&hw.reads==0);
    auto malformed=message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1);
    using RawEnum=__underlying_type(decltype(malformed.type));RawEnum bad=99;
    memcpy(&malformed.type,&bad,sizeof(bad));
    CHECK(rt.handle(malformed,req(10,4),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::InvalidCommand);
    CHECK(hw.inputs==0&&hw.reads==0);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<63),req(10,2),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Denied);
    CHECK(hw.inputs==0&&hw.reads==0);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,UINT64_C(1)<<1),req(10,3,1),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::WrongChannel);
    PASS("whole-mask-authorization-channel-and-63-safe");
  }
  {
    Mock hw;hw.caps={6,6,6,6};GpioModuleRuntime rt(hw);Sink sink;sink.accept=false;
    CHECK(rt.configure(withRemote(true),true,true,1u<<2));
    auto c=req(10,201);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,2),c,sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Queued);
    CHECK(hw.reads==1&&rt.pendingOutput());
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_READ_GPIOS,2),req(10,202),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Busy);
    CHECK(hw.reads==1);
    sink.accept=true;rt.poll(100,false,4,sendPacket,&sink);
    CHECK(!rt.pendingOutput());
    PASS("single-pending-reply-bounds-backpressure");
  }
  {
    Mock hw;hw.caps={6,6,6,6};GpioModuleRuntime rt(hw);Sink sink;
    CHECK(rt.configure(withRemote(true),true,true,(1u<<2)|(1u<<5)));
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_WRITE_GPIOS,4,4),req(10,301,5),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::Applied);
    CHECK(hw.writes==1);
    CHECK(rt.handle(message(meshtastic_HardwareMessage_Type_WRITE_GPIOS,4,4),req(10,302,4),sendPacket,&sink)==GpioModuleRuntime::HandleStatus::WrongChannel);
    CHECK(hw.writes==1);
    PASS("all-named-gpio-slots-accepted-only-on-bound-slots");
  }
  {
    Mock hw;hw.caps={UINT64_C(1)<<3,UINT64_C(1)<<3,UINT64_C(1)<<3,0};
    GpioModuleRuntime rt(hw);Sink sink;
    auto pullupSettings=withDetection(3);
    meshtastic_ModuleConfig pullup{};pullup.which_payload_variant=GpioModuleSettings::DetectionTag;
    pullup.payload_variant.detection_sensor=pullupSettings.detection();
    pullup.payload_variant.detection_sensor.use_pullup=true;CHECK(pullupSettings.set(pullup));
    CHECK(!rt.validateConfig(pullupSettings));
    CHECK(rt.configure(pullupSettings,true,false,0));
    rt.poll(0,false,4,sendPacket,&sink);
    CHECK(hw.inputs==0&&hw.reads==0); // pull-up is required by stock detector
    PASS("detection-requires-exported-pullup-capability");
  }
  {
    Mock hw;hw.caps={UINT64_C(1)<<3,UINT64_C(1)<<3,0,0};
    GpioModuleRuntime rt(hw);Sink sink;
    auto detectionSettings=withDetection(3);
    meshtastic_ModuleConfig named{};named.which_payload_variant=GpioModuleSettings::DetectionTag;
    named.payload_variant.detection_sensor=detectionSettings.detection();
    strcpy(named.payload_variant.detection_sensor.name,"d\xc3\xb6r");
    CHECK(detectionSettings.set(named));
    CHECK(rt.configure(detectionSettings,true,false,0));
    rt.poll(0,false,4,sendPacket,&sink);CHECK(hw.inputs==1&&hw.reads==0);
    rt.poll(5,false,4,sendPacket,&sink);CHECK(hw.reads==0);
    rt.poll(100,false,4,sendPacket,&sink);CHECK(hw.reads==1);
    rt.poll(1000,false,4,sendPacket,&sink);CHECK(hw.reads==2);
    hw.lastValue=UINT64_C(1)<<3;
    rt.poll(1100,false,4,sendPacket,&sink);CHECK(hw.reads==3);
    CHECK(sink.calls==1&&sink.last.portnum==meshtastic_PortNum_DETECTION_SENSOR_APP);
    CHECK(sink.last.to==UINT32_MAX&&!sink.last.wantAck);
    CHECK(sink.last.payloadSize>=5&&!memcmp(sink.last.payload,"d\xc3\xb6r",4));
    PASS("detection-configures-once-samples-100ms-and-broadcasts-port10");
  }
  {
    Mock hw;hw.caps={UINT64_C(1)<<3,UINT64_C(1)<<3,UINT64_C(1)<<3,UINT64_C(1)<<3};
    GpioModuleRuntime rt(hw);GpioModuleSettings s=withDetection(3);
    meshtastic_ModuleConfig m{};m.which_payload_variant=GpioModuleSettings::RemoteTag;
    auto &r=m.payload_variant.remote_hardware;r=meshtastic_ModuleConfig_RemoteHardwareConfig_init_zero;
    r.enabled=true;r.available_pins_count=1;r.available_pins[0].gpio_pin=3;
    r.available_pins[0].type=meshtastic_RemoteHardwarePinType_DIGITAL_WRITE;CHECK(s.set(m));
    CHECK(!rt.validateConfig(s));
    CHECK(hw.inputs==0&&hw.reads==0&&hw.writes==0);
    PASS("detector-and-remote-configured-pin-conflict-rejected-readonly");
  }
  {
    Mock hw;hw.caps={UINT64_C(1)<<3,UINT64_C(1)<<3,0,0};GpioModuleRuntime rt(hw);
    auto tooLong=withDetection(3);meshtastic_ModuleConfig m{};
    m.which_payload_variant=GpioModuleSettings::DetectionTag;
    m.payload_variant.detection_sensor=tooLong.detection();
    m.payload_variant.detection_sensor.minimum_broadcast_secs=UINT32_MAX;
    CHECK(tooLong.set(m));CHECK(!rt.validateConfig(tooLong));
    m.payload_variant.detection_sensor=tooLong.detection();
    m.payload_variant.detection_sensor.monitor_pin=64;
    CHECK(!tooLong.set(m));
    CHECK(hw.inputs==0&&hw.reads==0&&hw.writes==0);
    PASS("candidate-interval-and-pin-boundaries-rejected-without-io");
  }
  {
    Mock hw;hw.caps={UINT64_C(1)<<3,UINT64_C(1)<<3,0,0};
    GpioModuleRuntime rt(hw);Sink sink;auto valid=withDetection(3);
    CHECK(rt.configure(valid,true,false,0));rt.poll(0,false,4,sendPacket,&sink);
    CHECK(hw.inputs==1&&rt.available());
    auto invalid=valid;meshtastic_ModuleConfig m{};
    m.which_payload_variant=GpioModuleSettings::DetectionTag;
    m.payload_variant.detection_sensor=valid.detection();
    m.payload_variant.detection_sensor.minimum_broadcast_secs=UINT32_MAX;
    CHECK(invalid.set(m)&&invalid.valid()&&!rt.validateConfig(invalid));
    CHECK(rt.configure(invalid,true,false,0));CHECK(!rt.available());
    rt.poll(100,false,4,sendPacket,&sink);CHECK(hw.reads==0&&sink.calls==0);
    CHECK(rt.configure(valid,false,false,0));CHECK(!rt.available());
    CHECK(rt.configure(valid,true,false,0));CHECK(rt.available());
    rt.poll(200,false,4,sendPacket,&sink);CHECK(hw.inputs==2&&hw.reads==0);
    rt.poll(300,false,4,sendPacket,&sink);CHECK(hw.reads==1);
    PASS("unsupported-loaded-config-fails-closed-and-readiness-epoch-resets-input-mode");
  }
  printf("PASS groups=%u\n",groups);return 0;
}
