// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
// Inert storage double. Signing, validation and MCUboot trailer selection are
// owned by the shared Session implementation and its independently run tests.
namespace mt::update::signedOta {
inline std::vector<uint8_t> stored;
inline size_t expectedSize=24;
inline bool selected=false,rejectImage=false;
class Session {
  bool open_=false;
public:
  explicit Session(const uint8_t *hash){assert(hash&&hash[0]==0x11);}
  const char *command(const char *line){
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    assert(mt::ble::store::payloadAllowed());
#endif
    if(!strcmp(line,"VERSION"))return "OK 81 test 1 v2\n";
    if(std::string(line)!="OTA "+std::to_string(expectedSize)+" "+std::string(64,'1'))return "ERR Hash Rejected\n";
    open_=true;return "ERASING\nOK\n";
  }
  const char *write(const uint8_t *p,size_t n){
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    assert(mt::ble::store::payloadAllowed());
#endif
    assert(open_);stored.insert(stored.end(),p,p+n);
    if(stored.size()<expectedSize)return "ACK\n";
    open_=false;if(stored.size()!=expectedSize||rejectImage)return "ERR Hash Mismatch or Invalid Signed Image\n";
    selected=true;return "OK\n";
  }
  size_t imageSize()const{return expectedSize;}
  bool downloading()const{return open_;}
  bool complete()const{return selected;}
};
}
