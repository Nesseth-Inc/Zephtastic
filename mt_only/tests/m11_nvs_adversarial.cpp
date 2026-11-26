// SPDX-License-Identifier: GPL-3.0-only
// Independent raw namespace cases through the pinned actual NVS/settings engine.
#include "pki/ZephyrPkiStorage.h"
#include "bridge.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
using namespace mt::pki;
static void row(unsigned slot,const char *name,size_t n,const uint8_t *data,size_t size){assert(model_put(uint16_t(0x8000+slot),name,n)==ssize_t(n));assert(model_put(uint16_t(0xc000+slot),data,size)==ssize_t(size));model_counter(uint16_t(0x8000+slot));}
static void rejected(){NvsBootSettings boot;const auto writes=model_writes();std::array<uint8_t,32768> before,after;model_copy_flash(before.data());assert(boot.begin()==-EILSEQ);model_copy_flash(after.data());assert(before==after&&model_writes()==writes);}
int main(){
 for(unsigned sectors:{4u,8u}){
  // Old settings traversal repairs an orphan even when querying another subtree.
  // A private load-only policy must inspect before making that public query.
  model_reset(sectors);const char name[]="mtidentity/state0";assert(model_put(0x8001,name,sizeof(name)-1)==ssize_t(sizeof(name)-1));model_counter(0x8001);model_remount();rejected();const auto before=model_writes();(void)settings_get_val_len("mtbase/state0");assert(model_writes()>before);{NvsBootSettings afterRepair;assert(afterRepair.begin()==0);std::array<uint8_t,84> out;out.fill(0xa5);assert(afterRepair.read(name,out.data(),out.size())==-ENOENT);afterRepair.end();}
  // Adversarial encodings are not alternate aliases of a legitimate private key.
  for(unsigned variant=0;variant<5;++variant){model_reset(sectors);uint8_t value=9;if(variant==0){const char hidden[]="mtidentity/state0\0suffix";row(1,hidden,sizeof(hidden)-1,&value,1);}if(variant==1)model_counter(0x7fff);if(variant==2)model_counter(0xffff);if(variant==3){uint8_t count[3]={1,0x80,0};assert(model_put(0x8000,count,3)==3);}if(variant==4){char name97[97];memset(name97,'a',97);row(1,name97,97,&value,1);}rejected();}
  // Exact permitted maximum name is safe and does not imply private state.
  model_reset(sectors);char longest[96];memset(longest,'z',96);uint8_t value=7;row(1,longest,96,&value,1);{NvsBootSettings boot;const auto writes=model_writes(),reads=model_reads();assert(boot.begin()==0&&model_reads()-reads==1+2*NvsBootSettings::NameLimit&&model_writes()==writes);uint8_t out[84];memset(out,0xa5,sizeof(out));assert(boot.read("mtidentity/state0",out,sizeof(out))==-ENOENT);for(auto c:out)assert(c==0);boot.end();}
  // Counter-defined interior deletion holes are allowed, but a stale counter
  // hiding a live pair anywhere inside64 is never absence.
  model_reset(sectors);row(64,"unrelated/key",13,&value,1);{NvsBootSettings boot;assert(!boot.begin());boot.end();}model_counter(0x8001);rejected();
 }
 std::cout<<"PASS actual-settings-pre-read-repair-demonstrates-required-private-first-boot-order\n";
 std::cout<<"PASS raw-counter-length-name-alias-and-supported-limit-boundaries-no-boot-writes\n";
 std::cout<<"PASS interior-holes-accepted-hidden-live-row-under-stale-counter-rejected\n";
}
