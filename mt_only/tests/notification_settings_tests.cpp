// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/NotificationSettings.h"
#include "notifications/NotificationJournal.h"
#include "ModuleJournal.h"
#include "meshtastic/module_config.pb.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <type_traits>
using namespace mt;
static unsigned groups;
static void pass(const char *label){++groups;std::printf("PASS %s\n",label);}
int main(){
  {
    // Independent CRC16/CCITT-FALSE vectors (binascii.crc_hqx, init0xffff).
    const uint8_t expected[]={0x5a,0x54,0x4e,0x32,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf6,0x16};
    const uint8_t selected[]={0x5a,0x54,0x4e,0x31,0x0d,0x3f,0x78,0x56,0x34,0x12,0xff,0xff,0,0,0xa7,0x16};
    NotificationSettings s;assert(s.valid()&&!std::memcmp(s.bytes,expected,sizeof(expected)));
    assert(Config::checksum(reinterpret_cast<const uint8_t*>("123456789"),9)==0x29b1);
    std::memcpy(s.bytes,selected,sizeof(selected));assert(s.valid()&&s.enabled()&&!s.alertMessage()&&s.alertBell()&&s.active());
    assert(s.output()==63&&s.outputMs()==0x12345678&&s.nagTimeoutSeconds()==65535);
    pass("independent complete16-byte format and little-endian CRC/vector compatibility");
  }
  {
    NotificationSettings s;const auto before=s;
    assert(!s.enabled()&&!s.alertMessage()&&!s.alertBell()&&!s.active()&&!s.output()&&!s.outputMs()&&!s.nagTimeoutSeconds());
    assert(s.set(false,false,false,false,0,0,0)&&s.same(before));
    assert(s.set(true,true,true,true,0,0,0)&&s.valid()&&!s.outputMs()&&!s.nagTimeoutSeconds());
    assert(s.set(false,false,false,false,0,0,0)&&s.same(before));
    pass("disabled fresh bytes and explicit zero durations survive enable/disable without implied1000ms");
  }
  {
    using Nag=decltype(meshtastic_ModuleConfig_ExternalNotificationConfig{}.nag_timeout);
    static_assert(sizeof(Nag)==2&&std::is_same<Nag,uint16_t>::value);
    static_assert(NotificationSettings::MaxNagTimeoutSeconds==std::numeric_limits<Nag>::max());
    NotificationSettings s;assert(s.set(true,false,true,false,63,NotificationSettings::MaxOutputMs,NotificationSettings::MaxNagTimeoutSeconds));
    const auto before=s;
    for(uint32_t pin:{64u,256u,UINT32_MAX})assert(!s.set(false,true,false,true,pin,1,1)&&s.same(before));
    for(uint32_t ms:{0x80000000u,UINT32_MAX})assert(!s.set(false,true,false,true,1,ms,1)&&s.same(before));
    for(uint32_t sec:{NotificationSettings::MaxNagTimeoutSeconds+1,UINT32_MAX})assert(!s.set(false,true,false,true,1,1,sec)&&s.same(before));
    meshtastic_ModuleConfig_ExternalNotificationConfig wire{};wire.nag_timeout=std::numeric_limits<Nag>::max();
    assert(s.nagTimeoutSeconds()==wire.nag_timeout&&s.bytes[10]==0xff&&s.bytes[11]==0xff&&!s.bytes[12]&&!s.bytes[13]);
    auto beyondWire=before;Config::put32(beyondWire.bytes+10,65536);beyondWire.seal();assert(!beyondWire.valid());
    assert(!s.set(false,false,false,false,0,0,65536)&&s.same(before));
    assert(s.output()==63&&s.outputMs()==uint32_t(INT32_MAX)&&s.nagTimeoutSeconds()*1000u<=uint32_t(INT32_MAX));
    pass("generic pin and signed-millisecond timer bounds reject without partial assignment or uint8 truncation");
  }
  {
    NotificationSettings original;assert(original.set(true,true,false,false,7,1234,99));
    for(unsigned bit:{0x20u,0x40u,0x80u}){auto bad=original;bad.bytes[4]|=uint8_t(bit);bad.seal();assert(!bad.valid());}
    for(unsigned offset:{0u,1u,2u,3u}){auto bad=original;bad.bytes[offset]^=1;bad.seal();assert(!bad.valid());}
    auto bad=original;bad.bytes[5]=64;bad.seal();assert(!bad.valid());
    bad=original;Config::put32(bad.bytes+6,0x80000000u);bad.seal();assert(!bad.valid());
    bad=original;Config::put32(bad.bytes+10,NotificationSettings::MaxNagTimeoutSeconds+1);bad.seal();assert(!bad.valid());
    pass("valid CRC cannot admit unknown flags, alternate magic/version or out-of-range fields");
  }
  {
    NotificationSettings original;assert(original.set(true,false,true,true,31,65536,0));
    for(unsigned offset=0;offset<NotificationSettings::Size;++offset){auto bad=original;bad.bytes[offset]^=1;assert(!bad.valid());}
    auto copy=original;assert(copy.same(original));copy.bytes[0]='X';const auto before=copy;
    assert(!copy.set(false,false,false,false,0,0,0)&&copy.same(before));
    assert(original.valid()&&!original.same(copy));
    pass("every single-byte corruption fails integrity; copying and failed format edits preserve original RAM");
  }
  {
    static_assert(sizeof(NotificationSettings)==16&&std::is_trivially_copyable<NotificationSettings>::value);
    static_assert(sizeof(ModuleSettings)==112&&ModuleJournal::RecordSize==128&&Config::Size==640&&SettingsJournal::RecordSize==656);
    static_assert(NotificationJournal::RecordSize==32);
    ModuleSettings old;meshtastic_ModuleConfig got{};assert(old.get(got,meshtastic_ModuleConfig_telemetry_tag));const auto before=old;
    assert(old.set(got)&&old.same(before));NotificationSettings n;assert(n.set(true,true,true,false,9,10,11));assert(old.same(before));
    pass("new16/32 records coexist with unchanged112/128 module and640/656 core layouts");
  }
  std::printf("NOTIFICATION_SETTINGS_GROUPS %u\n",groups);
}
