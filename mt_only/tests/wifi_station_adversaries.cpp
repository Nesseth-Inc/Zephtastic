#include "../src/network/WifiStationSettings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using S=mt::network::WifiStationSettings;
static void seal(S&s){auto c=mt::Config::checksum(s.bytes,106);s.bytes[106]=uint8_t(c);s.bytes[107]=uint8_t(c>>8);}
static void refused(S&s){S old=s;assert(!s.setEnabled(true));assert(!s.set(false,"A",1,"B",1));assert(!memcmp(old.bytes,s.bytes,S::Size));}
int main(){
 S s;assert(s.valid()&&!s.enabled()&&s.bytes[106]==0x8a&&s.bytes[107]==0xed);
 assert(!s.setEnabled(true)&&s.valid());assert(s.set(true,"A",1,"B",1));assert(s.bytes[106]==0x34&&s.bytes[107]==0xb6);
 S old=s;assert(s.setEnabled(false)&&s.valid()&&!s.enabled());assert(!memcmp(s.bytes+5,old.bytes+5,101));assert(s.setEnabled(true));
 const char*sp=reinterpret_cast<const char*>(s.bytes+8);const char*pp=reinterpret_cast<const char*>(s.bytes+41);assert(s.set(true,sp,1,pp,1)&&!memcmp(s.bytes,old.bytes,S::Size));
 char ss[33],ps[65];memset(ss,'S',sizeof(ss));memset(ps,'P',sizeof(ps));assert(s.set(true,ss,32,ps,64)&&s.valid());old=s;
 assert(!s.set(true,ss,33,ps,64)&&!memcmp(s.bytes,old.bytes,S::Size));assert(!s.set(true,ss,32,ps,65)&&!memcmp(s.bytes,old.bytes,S::Size));
 assert(!s.set(true,nullptr,1,"B",1)&&!s.set(true,"A",1,nullptr,1)&&!memcmp(s.bytes,old.bytes,S::Size));
 char embedded[3]={'A',0,'C'};assert(!s.set(true,embedded,3,"B",1)&&!s.set(true,"A",1,embedded,3)&&!memcmp(s.bytes,old.bytes,S::Size));
 assert(s.set(false,nullptr,0,nullptr,0)&&s.valid()&&!s.enabled());assert(!s.setEnabled(true));
 const unsigned offsets[]={0u,4u,5u,6u,7u,8u,40u,41u,105u};
 for(unsigned off: offsets){S b;assert(b.set(true,"A",1,"B",1));if(off==8||off==41)b.bytes[off]=0;else b.bytes[off]^=0x80;seal(b);assert(!b.valid());refused(b);}
 S crc;assert(crc.set(true,"A",1,"B",1));crc.bytes[106]^=1;assert(!crc.valid());refused(crc);
 // Invalid record getters must stay closed at the integrated boundary.
 assert(!crc.enabled() && crc.ssidLength()==0 && crc.pskLength()==0);
 puts("PASS_ROOT_RECORD_CANARY_DEFAULTS_GOLDENS_BOUNDS_ALIASING_CORRUPTION_AND_PRESERVATION;INVALID_GETTERS_CLOSED");
}
