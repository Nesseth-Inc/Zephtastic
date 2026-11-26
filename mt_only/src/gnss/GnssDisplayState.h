// SPDX-License-Identifier: GPL-3.0-only
// Qwen M108 display-state proposal, with an explicit byte-sized UI enum.
#pragma once
#include <stdint.h>
namespace zt::gnss {
enum class DisplayState:uint8_t {Absent,Off,Starting,Search,Fix,Fault};
constexpr DisplayState classifyDisplayState(bool fitted,bool enabled,bool ready,bool fault,bool freshFix){
  if(!fitted)return DisplayState::Absent;
  if(fault)return DisplayState::Fault;
  if(!enabled)return DisplayState::Off;
  if(!ready)return DisplayState::Starting;
  return freshFix?DisplayState::Fix:DisplayState::Search;
}
}
