// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>

namespace mt::stock::paxcounter {
// libpax 6f52ee9, libpax.cpp::mac_add, libpax_api.cpp::report(mode=0).
// The shared 16-bit bucket retains neither a complete address nor a frame.
struct Interval {
  uint8_t seen[65536 / 8]{};
  uint16_t wifi=0, ble=0;
  uint16_t reportedWifi=0, reportedBle=0;
  uint32_t start=0;
  static bool permitted(bool bluetoothEnabled,bool wifiEnabled){return !bluetoothEnabled&&!wifiEnabled;}
  static uint32_t reportPeriod(uint32_t requested,bool router){return requested?requested:(router?43200u:3600u);}

  static bool bucket(const uint8_t *address, bool littleEndian, uint16_t &id) {
    if(!address || !(address[littleEndian ? 5 : 0] & 2))return false;
    id=littleEndian ? uint16_t(address[1] | uint16_t(address[0])<<8)
                    : uint16_t(address[4] | uint16_t(address[5])<<8);
    return true;
  }
  bool add(uint16_t id,bool isWifi) {
    uint8_t &word=seen[id>>3];const uint8_t mask=uint8_t(1u<<(id&7));
    if(word&mask)return false;
    word|=mask;
    if(isWifi)++wifi;else ++ble;
    return true;
  }
  bool advance(uint32_t now,uint32_t period) {
    const uint32_t elapsed=now-start;
    if(int32_t(elapsed)<0 || elapsed<period)return false;
    const uint32_t windows=elapsed/period;
    reportedWifi=windows==1?wifi:0;reportedBle=windows==1?ble:0;
    start+=windows*period;
    memset(seen,0,sizeof(seen));wifi=ble=0;return true;
  }
  void reset(uint32_t now) {
    memset(seen,0,sizeof(seen));wifi=ble=reportedWifi=reportedBle=0;start=now;
  }
};
}
