// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WifiStationEdit.h"
#include "WifiStationConfigMapping.h"
#include "WifiStationConfigExport.h"
#include "../SettingsEdit.h"
#include "../RebootSchedule.h"

namespace mt::network {
// Main/service-lock owner only. Stock NetworkConfig takes effect on reboot.
// A failed multi-journal commit never schedules it. A later successful commit
// still notices a WiFi journal saved before another journal failed.
class WifiStationAdmin {
  WifiStationEdit &wifi_;SettingsEdit &core_;RebootSchedule &reboot_;
  WifiStationSettings boot_;bool loaded_=false;
public:
  WifiStationAdmin(WifiStationEdit &wifi,SettingsEdit &core,RebootSchedule &reboot):
    wifi_(wifi),core_(core),reboot_(reboot){}
  bool loaded(){if(loaded_||!wifi_.ready())return false;boot_=wifi_.config();loaded_=true;return true;}
  bool get(meshtastic_Config_NetworkConfig &out)const {
    return loaded_&&wifi_.ready()&&WifiStationConfigExport::toConfig(wifi_.config(),out);
  }
  bool changedFromBoot()const {
    return loaded_&&!boot_.same(wifi_.config());
  }
  int afterCommit(int result,uint64_t now) {
    if(!result&&!core_.open()&&!wifi_.dirty()&&changedFromBoot()&&reboot_.action()==ScheduledOwnerAction::None)
      reboot_.request(1,now);
    return result;
  }
  int save(const meshtastic_Config_NetworkConfig &input,bool capable,uint32_t now,uint64_t monotonic) {
    if(!loaded_||!wifi_.ready())return -EIO;
    if(reboot_.pending())return -EBUSY;
    WifiStationSettings candidate;
    if(!WifiStationConfigMapping::fromConfig(input,capable,wifi_.config(),candidate))return -EINVAL;
    const int result=wifi_.save(candidate,now,core_.open());
    if(!result)core_.activity(now);
    return afterCommit(result,monotonic);
  }
};
}
