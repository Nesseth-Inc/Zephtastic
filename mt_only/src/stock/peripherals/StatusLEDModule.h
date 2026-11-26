// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, StatusLEDModule's LoRa receive flash.
#pragma once
#include <stdint.h>

namespace mt::stock {
class StatusLedRadio {
    static constexpr uint32_t FlashMs=100;
    uint32_t started_=0;
    bool on_=false;
public:
    void handleLoRaRx(uint32_t now) {started_=now;on_=true;}
    bool runOnce(uint32_t now) {
        if(on_&&uint32_t(now-started_)>=FlashMs)on_=false;
        return on_;
    }
};
}
