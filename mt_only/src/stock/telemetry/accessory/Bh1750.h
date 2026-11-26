// SPDX-License-Identifier: GPL-3.0-only
// Stock BH1750Sensor, Wolfgang Ewald's BH1750_WE 1.1.10.
#pragma once
#include "SensorBus.h"
namespace mt::stock::telemetry::accessory {
class Bh1750 {
    Bus &bus;
    enum Stage : uint8_t { Idle, Timing, First, Running } stage = Idle;
    uint64_t readyAt = 0;
    bool powered = false;
    int command(uint8_t op) { return bus.write(&op,1); }
    bool start() {
        // The shared stock address can also contain an LTR553. BH1750 itself has no ID.
        uint8_t reg=0x86, id=0;
        if (!bus.write(&reg,1)) {
            bus.wait(20);
            if (!bus.read(&id,1) && id==0x92) return false;
        }
        if (command(0x01)) return false;
        powered=true;
        // CHM, MTreg factor 1.0 (69). Both CHM writes occur in the pinned init.
        if (command(0x10) || command(0x10) || command(0x42) || command(0x65)) { stop(); return false; }
        readyAt=bus.now()+200;
        stage=Timing;
        return false;
    }
public:
    explicit Bh1750(Bus &p):bus(p) {}
    void stop() {
        stage=Idle;
        if (powered && !command(0)) powered=false;
    }
    bool sample(float &lux) {
        if (stage==Idle) {
            if (powered) { stop(); if (powered) return false; }
            return start();
        }
        if (stage!=Running && bus.now()<readyAt) return false;
        if (stage==Timing) {
            if (command(0x10)) { stop(); return false; }
            readyAt=bus.now()+180; // Allow the first maximum-duration high-resolution conversion.
            stage=First;
            return false;
        }
        uint8_t bytes[2];
        if (bus.read(bytes,2)) { stop(); return false; }
        stage=Running;
        lux=float((uint16_t(bytes[0])<<8 | bytes[1])/1.2);
        return true;
    }
};
}
