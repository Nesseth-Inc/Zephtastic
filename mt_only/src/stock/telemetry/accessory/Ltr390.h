// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic LTR390UVSensor, with the register definitions from Adafruit LTR390 1.1.2.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
class Ltr390 {
    Bus &bus;
    bool initialized=false, powerMayBeOn=false;
    float visible=0, ultraviolet=0;

    bool initialize() {
        uint8_t id, control;
        if (bus.readReg(6,&id,1) || (id>>4)!=0xb || bus.readReg(0,&control,1)) return false;
        // The chip may reset before it acknowledges. Check its state after the delay.
        (void)bus.writeReg(0,control|0x10);
        bus.wait(10);
        // Some nRF52 controllers need bus recovery after the reset NACK.
        if (bus.readReg(0,&control,1) &&
            (bus.recover() || bus.readReg(0,&control,1))) return false;
        if (control&0x10) return false;
        powerMayBeOn=true;
        if (bus.update(0,0x0a,0x0a) || bus.update(5,7,4) || bus.update(4,0x70,0)) return false;
        if (bus.readReg(0,&control,1) || (control&0x0a)!=0x0a) return false;
        visible=ultraviolet=0;
        initialized=true;
        return true;
    }
public:
    explicit Ltr390(Bus &device):bus(device){}
    void stop() {
        initialized=false;
        if (powerMayBeOn && !bus.update(0,2,0)) powerMayBeOn=false;
    }
    bool sample(float &lux,float &uvIndex) {
        if (!initialized && !initialize()) return false;
        uint8_t status,control,bytes[3];
        if (bus.readReg(7,&status,1) || !(status&8) || bus.readReg(0,&control,1)) return false;
        if (!(control&2)) {initialized=false;return false;}
        const bool uvMode=(control&8)!=0;
        if (bus.readReg(uvMode?0x10:0x0d,bytes,sizeof(bytes))) return false;
        const uint32_t count=uint32_t(bytes[2])<<16 | uint32_t(bytes[1])<<8 | bytes[0];
        if (bus.update(5,7,uvMode?0:4) || bus.update(0,8,uvMode?0:8)) {
            initialized=false;
            return false;
        }
        if (uvMode) ultraviolet=count/2300.f;
        else visible=0.6*count/4;
        lux=visible;
        uvIndex=ultraviolet;
        return true;
    }
};
}
