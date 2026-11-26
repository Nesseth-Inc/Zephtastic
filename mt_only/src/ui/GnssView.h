// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../gnss/GnssDisplayState.h"
#include <stdio.h>
namespace mt::ui {
struct GnssView {
  zt::gnss::DisplayState state=zt::gnss::DisplayState::Absent;
  uint16_t satellites=0;
  void label(char *out,size_t capacity)const{
    using State=zt::gnss::DisplayState;
    const char *text="GPS N/A";
    switch(state){
    case State::Absent:break;
    case State::Off:text="GPS OFF";break;
    case State::Starting:text="GPS STARTING";break;
    case State::Search:text="GPS SEARCH";break;
    case State::Fix:snprintf(out,capacity,"GPS FIX %u SAT",unsigned(satellites));return;
    case State::Fault:text="GPS FAULT";break;
    }
    snprintf(out,capacity,"%s",text);
  }
};
}
