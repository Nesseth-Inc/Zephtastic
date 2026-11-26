// SPDX-License-Identifier: GPL-3.0-only
#include "pki/UsbContactsCommand.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

using namespace mt::pki;

int main(){
  const std::string prefix="zt_pki_contacts_enable_v1 ";
  const std::string valid=prefix+"1234abcd "+std::string(64,'a');
  assert(valid.size()==detail::kContactsEnableFrameSize);
  UsbStorageCommand parsed;parsed.localNode=0xdeadbeef;memset(parsed.localPublic,0x5a,sizeof(parsed.localPublic));
  assert(UsbContactsParser::parse(reinterpret_cast<const uint8_t *>(valid.data()),valid.size(),parsed));
  assert(parsed.localNode==0x1234abcdu);
  for(uint8_t byte:parsed.localPublic)assert(byte==0xaa);

  const UsbStorageCommand unchanged=parsed;
  auto reject=[&](const std::string &input){
    assert(!UsbContactsParser::parse(reinterpret_cast<const uint8_t *>(input.data()),input.size(),parsed));
    assert(parsed.localNode==unchanged.localNode&&
           memcmp(parsed.localPublic,unchanged.localPublic,sizeof(parsed.localPublic))==0);
  };
  reject(valid+" ");
  reject(valid.substr(0,valid.size()-1));
  std::string bad=valid;bad[0]='Z';reject(bad);
  bad=valid;bad[prefix.size()+7]='A';reject(bad);
  bad=valid;bad[prefix.size()+8]='\t';reject(bad);
  bad=valid;bad.back()='G';reject(bad);
  bad=valid;bad.replace(prefix.size(),8,"00000001");reject(bad);
  assert(!UsbContactsParser::parse(nullptr,valid.size(),parsed));
  puts("PASS_M154_CONTACTS_COMMAND");
}
