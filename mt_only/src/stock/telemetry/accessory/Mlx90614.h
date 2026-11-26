// SPDX-License-Identifier: GPL-3.0-only
// Pinned Meshtastic MLX90614 health producer and Adafruit MLX90614 2.1.6.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
class Mlx90614 {
    Bus &bus;
    bool identified = false;

    int word(uint8_t command, uint16_t &value) {
        uint8_t bytes[3];
        const int error = bus.readReg(command, bytes, sizeof(bytes));
        if (error) return error;
        // SMBus packet error code covers both addresses and the command.
        const uint8_t message[] = {0xb4, command, 0xb5, bytes[0], bytes[1]};
        uint8_t pec = 0;
        for (uint8_t octet : message) {
            pec ^= octet;
            for (unsigned bit = 0; bit < 8; ++bit)
                pec = (pec & 0x80) ? (pec << 1) ^ 7 : pec << 1;
        }
        if (pec != bytes[2]) return -EBADMSG;
        value = uint16_t(bytes[1]) << 8 | bytes[0];
        return 0;
    }
    int identify() {
        uint16_t value;
        int error = word(0x2e, value); // EEPROM SMBus-address cell, command 0x20 + 0x0e.
        if (error || (value & 0xff) != 0x5a) return error ? error : -ENODEV;
        uint16_t anyBits = 0, allBits = UINT16_MAX;
        for (unsigned command = 0x3c; command <= 0x3f; ++command) {
            if ((error = word(command, value))) return error;
            anyBits |= value;
            allBits &= value;
        }
        if (!anyBits || allBits == UINT16_MAX) return -ENODEV;
        identified = true;
        return 0;
    }
public:
    explicit Mlx90614(Bus &device) : bus(device) {}
    void stop() { identified = false; }
    int measure(float &objectCelsius) {
        if (!identified) {
            const int error = identify();
            if (error) return error;
        }
        uint16_t emissivity, raw;
        int error = word(0x24, emissivity);
        if (!error) {
            const double setting = double(emissivity) / 65535.0;
            // The stock human-skin setting is 0.98 with a tolerance of 0.001.
            // Provisioning the EEPROM is variant-specific and requires a sensor restart.
            if (setting < 0.979 || setting > 0.981) return -ERANGE;
            error = word(0x07, raw);
        }
        if (error) { stop(); return error; }
        if (!raw || (raw & 0x8000)) return -ENODATA;
        // Keep the two float roundings of Adafruit's readTemp.
        float temp = raw;
        temp *= 0.02;
        temp -= 273.15;
        objectCelsius = temp;
        return 0;
    }
};
}
