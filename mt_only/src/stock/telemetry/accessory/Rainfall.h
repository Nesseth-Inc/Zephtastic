// SPDX-License-Identifier: GPL-3.0-only
// DFRobot SEN0575, register transactions from DFRobot_RainfallSensor 38fea5e.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
class Rainfall {
    Bus &bus;
    bool read(uint8_t reg,uint8_t (&bytes)[4]){
        return !bus.write(&reg,1) && !bus.read(bytes,sizeof(bytes));
    }
    bool window(uint8_t hours,float &millimeters){
        if(bus.writeReg(0x26,hours))return false;
        bus.wait(100);
        uint8_t bytes[4];
        if(!read(0x0c,bytes))return false;
        const uint32_t units=uint32_t(bytes[3])<<24 | uint32_t(bytes[2])<<16 |
                             uint32_t(bytes[1])<<8 | bytes[0];
        millimeters=units/10000.0;
        return true;
    }
public:
    explicit Rainfall(Bus &device):bus(device){}
    bool sample(float &oneHour,float &oneDay){
        uint8_t id[4];
        // This address is shared with a possible DS248X. Identify before issuing a command.
        if(!read(0,id) || id[0]!=0xc0 || id[1]!=0 || id[2]!=0x43 || id[3]!=0x73)return false;
        float hour,day;
        if(!window(1,hour) || !window(24,day))return false;
        oneHour=hour;
        oneDay=day;
        return true;
    }
};
}
