// SPDX-License-Identifier: GPL-3.0-only
// Pinned Meshtastic PowerStressModule.cpp. The owner supplies a monotonic
// clock and executes actions after releasing the radio/service mutex.
#pragma once
#include "meshtastic/powermon.pb.h"
#include <stdint.h>
#include <errno.h>

namespace mt::stock::powerstress {
using Opcode=meshtastic_PowerStressMessage_Opcode;
enum class Action { None, Info, BluetoothOn, BluetoothOff, Busy, Sleep, Mark };
struct Step {Action action; Opcode opcode; uint32_t remainingMs;bool started=false;};
class Module {
    uint32_t infoId_=0,lastId_=0,lastDuration_=0,duration_=0,startedAt_=0;
    Opcode lastOp_=meshtastic_PowerStressMessage_Opcode_UNSET,op_=meshtastic_PowerStressMessage_Opcode_UNSET;
    bool running_=false,info_=false;
public:
    void reset(){*this={};}
    int receive(uint32_t id,const meshtastic_PowerStressMessage &message) {
        if(!id || !(message.num_seconds>=0 && message.num_seconds<=2147483.0f))return -EINVAL;
        const uint32_t ms=uint32_t(message.num_seconds*1000.0);
        if(message.cmd==meshtastic_PowerStressMessage_Opcode_PRINT_INFO){
            if(id==lastId_)return -EINVAL;
            if(id!=infoId_){info_=true;infoId_=id;}
            return 0;
        }
        if(id==infoId_)return -EINVAL;
        if(id==lastId_)return message.cmd==lastOp_ && ms==lastDuration_ ? 0 : -EINVAL;
        switch(message.cmd){
        case meshtastic_PowerStressMessage_Opcode_LED_ON:case meshtastic_PowerStressMessage_Opcode_LED_OFF:
        case meshtastic_PowerStressMessage_Opcode_GPS_ON:case meshtastic_PowerStressMessage_Opcode_GPS_OFF:
        case meshtastic_PowerStressMessage_Opcode_LORA_OFF:case meshtastic_PowerStressMessage_Opcode_LORA_RX:
        case meshtastic_PowerStressMessage_Opcode_LORA_TX:case meshtastic_PowerStressMessage_Opcode_SCREEN_ON:
        case meshtastic_PowerStressMessage_Opcode_SCREEN_OFF:case meshtastic_PowerStressMessage_Opcode_CPU_IDLE:
        case meshtastic_PowerStressMessage_Opcode_CPU_FULLON:case meshtastic_PowerStressMessage_Opcode_CPU_DEEPSLEEP:
        case meshtastic_PowerStressMessage_Opcode_BT_ON:case meshtastic_PowerStressMessage_Opcode_BT_OFF:
            if(op_!=meshtastic_PowerStressMessage_Opcode_UNSET)return -EBUSY;
            op_=message.cmd;duration_=ms;running_=false;
            break;
        default:return -EINVAL;
        }
        lastId_=id;lastOp_=message.cmd;lastDuration_=ms;
        return 0;
    }
    Step poll(uint32_t now) {
        if(info_){info_=false;return {Action::Info,meshtastic_PowerStressMessage_Opcode_PRINT_INFO,0};}
        const auto op=op_;
        if(op==meshtastic_PowerStressMessage_Opcode_UNSET)return {Action::None,op,0};
        const bool first=!running_;
        if(first){startedAt_=now;running_=true;}
        const uint32_t elapsed=now-startedAt_;
        if(!first && elapsed>=duration_){op_=meshtastic_PowerStressMessage_Opcode_UNSET;return {Action::Mark,op_,0};}
        const uint32_t remaining=duration_-elapsed;
        switch(op) {
        case meshtastic_PowerStressMessage_Opcode_CPU_FULLON:return {Action::Busy,op,remaining,first};
        case meshtastic_PowerStressMessage_Opcode_CPU_DEEPSLEEP:return {Action::Sleep,op,remaining,first};
        case meshtastic_PowerStressMessage_Opcode_BT_ON:return {first?Action::BluetoothOn:Action::None,op,remaining,first};
        case meshtastic_PowerStressMessage_Opcode_BT_OFF:return {first?Action::BluetoothOff:Action::None,op,remaining,first};
        default:return {first?Action::Mark:Action::None,op,remaining,first};
        }
    }
};
}
