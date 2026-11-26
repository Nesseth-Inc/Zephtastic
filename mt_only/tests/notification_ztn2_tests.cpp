// M110 NotificationSettings v1(ZTN1)->v2(ZTN2) golden/adversarial fixture.
// This suite tests inner records; journal persistence is tested separately.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cstdio>
#include "notifications/NotificationSettings.h"
using namespace mt;
namespace {
// Literal ZTN1: flags 0x0D => enabled,!message,bell,active; output=37,
// output_ms=12345 LE, nag=600 LE, CRC=0x7A51 LE.
const uint8_t kV1[16]={'Z','T','N','1',0x0D,37,0x39,0x30,0,0,0x58,2,0,0,0x51,0x7A};
constexpr uint16_t kCrc=0x7A51;
void rejectWithValidCrc(const uint8_t* base,const std::function<void(NotificationSettings&)>& mutate){
  NotificationSettings bad;std::memcpy(bad.bytes,base,16);mutate(bad);bad.seal();assert(!bad.valid());
  const NotificationSettings before=bad;
  assert(!bad.setHeartbeatDisabled(false)&&bad.same(before));
  assert(!bad.set(false,false,false,false,0,0,0)&&bad.same(before));
}
}
void m110_fixture_notification_settings_v1v2(){
  static_assert(NotificationSettings::Size==16,"Size");
  // (1) Literal v1 checksum/layout and exact old getter values.
  NotificationSettings v1;std::memcpy(v1.bytes,kV1,16);
  assert(Config::checksum(v1.bytes,14)==kCrc&&v1.bytes[14]==0x51&&v1.bytes[15]==0x7A);
  assert(v1.valid()&&v1.bytes[3]=='1'&&v1.bytes[4]==0x0D);
  assert(v1.heartbeatDisabled()); // all valid v1 records imply disabled
  assert(v1.enabled()&&!v1.alertMessage()&&v1.alertBell()&&v1.active());
  assert(v1.output()==37&&v1.outputMs()==12345u&&v1.nagTimeoutSeconds()==600u);
  // (2) Old setter migrates v1->v2 while carrying implied disabled=true.
  NotificationSettings m;std::memcpy(m.bytes,v1.bytes,16);
  assert(m.set(true,false,true,true,37,12345,600));
  assert(m.valid()&&m.bytes[3]=='2'&&m.bytes[4]==0x1D&&m.heartbeatDisabled());
  for(int i=5;i<=13;++i)assert(m.bytes[i]==v1.bytes[i]);
  assert(m.enabled()&&!m.alertMessage()&&m.alertBell()&&m.active());
  // (3) Explicit false promotes v1 to enabled heartbeat; old setter preserves it.
  NotificationSettings h;std::memcpy(h.bytes,v1.bytes,16);
  assert(h.setHeartbeatDisabled(false));
  assert(h.valid()&&h.bytes[3]=='2'&&h.bytes[4]==0x0D&&!h.heartbeatDisabled());
  for(int i=5;i<=13;++i)assert(h.bytes[i]==v1.bytes[i]);
  assert(h.set(true,false,true,true,37,12345,600));
  assert(h.bytes[4]==0x0D&&!h.heartbeatDisabled());
  assert(h.setHeartbeatDisabled(true)&&h.bytes[4]==0x1D&&h.heartbeatDisabled());
  // (4) Structural adversaries have fresh CRCs; corrupt CRC is separate.
  for(int b=5;b<=7;++b)rejectWithValidCrc(m.bytes,[b](NotificationSettings&s){s.bytes[4]|=uint8_t(1u<<b);});
  rejectWithValidCrc(v1.bytes,[](NotificationSettings&s){s.bytes[4]|=0x10;}); // v1 upper nibble reserved
  rejectWithValidCrc(v1.bytes,[](NotificationSettings&s){s.bytes[3]='3';});
  rejectWithValidCrc(v1.bytes,[](NotificationSettings&s){s.bytes[0]='X';});
  NotificationSettings corrupt;std::memcpy(corrupt.bytes,v1.bytes,16);corrupt.bytes[14]^=0xFF;assert(!corrupt.valid());
  NotificationSettings frozen;std::memcpy(frozen.bytes,v1.bytes,16);const auto before=frozen;
  assert(!frozen.set(true,false,true,true,64,12345,600)&&frozen.same(before));
}
int main(){
  m110_fixture_notification_settings_v1v2();
  NotificationSettings v2;assert(v2.valid()&&v2.heartbeatDisabled());
  assert(v2.setHeartbeatDisabled(false)&&!v2.heartbeatDisabled());
  v2.bytes[14]^=1;assert(!v2.valid()&&v2.heartbeatDisabled());
  const auto before=v2;assert(!v2.setHeartbeatDisabled(false)&&v2.same(before));
  puts("PASS literal v1 migration, preference preservation, structural/CRC refusal and invalid-v2 safe getter");
  puts("NOTIFICATION_ZTN2_GROUPS 1");
}
