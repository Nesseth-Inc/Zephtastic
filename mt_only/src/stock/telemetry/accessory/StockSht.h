// SPDX-License-Identifier: GPL-3.0-only
// Sensirion arduino-sht v1.2.6. See docs/licenses/Sensirion-arduino-sht.txt.
#pragma once
#include "SensorBus.h"

namespace mt::stock::telemetry::accessory {
enum class ShtModel : uint8_t { None, Sht4, Sht2, Sht3, Sht3Alt, ShtC };
struct ShtSample {
    float temperature = 0, humidity = 0;
    bool present = false;
};

// The pinned SHTXX owner auto-detects one sensor on the bus. Only addresses
// declared by this board are touched, in the library's actual discovery order.
class StockSht {
  public:
    StockSht(Bus *addr44, Bus *addr40, Bus *addr45, Bus *addr70)
        : candidates{addr44, addr40, addr44, addr45, addr70} {}
    ShtModel model() const { return chosen; }
    void stop() { chosen = ShtModel::None; }
    static bool adjustable(ShtModel m) {
        return m == ShtModel::Sht3 || m == ShtModel::Sht3Alt || m == ShtModel::Sht4;
    }
    int measure(unsigned accuracy, ShtSample &out) {
        if (accuracy > 2) return -EINVAL;
        if (chosen != ShtModel::None) return sample(chosen, *candidates[unsigned(chosen) - 1], accuracy, out);
        for (unsigned i = 0; i < 5; i++) {
            Bus *bus = candidates[i];
            if (!bus) continue;
            const auto candidate = ShtModel(i + 1);
            if (candidate == ShtModel::Sht2 && !serialMatches(*bus)) continue;
            if (candidate == ShtModel::Sht3 || candidate == ShtModel::Sht3Alt ||
                candidate == ShtModel::Sht4) {
                uint8_t id[2];
                // Stock scanner assigns TI's 0x5449 manufacturer ID at 0x44/45 to OPT3001.
                if (!bus->readReg(0x7e, id, 2) && id[0] == 0x54 && id[1] == 0x49) continue;
            }
            bus->wait(40);
            ShtSample detection;
            if (sample(candidate, *bus, 2, detection)) continue;
            chosen = candidate;
            // The library validates a second sample after auto-detection. Validate again
            // before the separate SHTXXSensor::getMetrics read; no test reading is reported.
            int rc = sample(chosen, *bus, 2, detection);
            if (rc) {
                stop();
                return rc;
            }
            return sample(chosen, *bus, accuracy, out);
        }
        return -ENODEV;
    }

  private:
    Bus *candidates[5];
    ShtModel chosen = ShtModel::None;
    static uint8_t crc(const uint8_t *p, unsigned n, uint8_t init) {
        for (unsigned i = 0; i < n; i++) {
            init ^= p[i];
            for (unsigned bit = 0; bit < 8; bit++)
                init = uint8_t((init << 1) ^ ((init & 0x80) ? 0x31 : 0));
        }
        return init;
    }
    static int read(Bus &b, unsigned cmd, unsigned bytes, unsigned wait, uint8_t *reply, unsigned n) {
        const uint8_t data[] = {uint8_t(cmd >> 8), uint8_t(cmd)};
        int rc = b.write(data + 2 - bytes, bytes);
        if (rc) return rc;
        if (wait) b.wait(wait);
        return b.read(reply, n);
    }
    static bool serialMatches(Bus &b) {
        uint8_t a[8], c[6], id[2];
        if (!b.readReg(0xfe, id, 2)) {
            if (id[0] == 0x54 && id[1] == 0x49) return false;
            if (id[0] == 0x19 && id[1] == 0x0f && !b.readReg(0xff, id, 2) && id[0] == 0 && id[1] == 0)
                return false;
        }
        if (!b.readReg(0x3d, id, 1) && id[0] == 0x72 && !b.readReg(0x3e, id, 1) && id[0] == 0x10)
            return false;
        if (read(b, 0xfa0f, 2, 0, a, sizeof(a)) || read(b, 0xfcc9, 2, 0, c, sizeof(c))) return false;
        for (unsigned i = 0; i < 8; i += 2)
            if (crc(a + i, 1, 0) != a[i + 1]) return false;
        return crc(c, 2, 0) == c[2] && crc(c + 3, 2, 0) == c[5];
    }
    static int sample(ShtModel model, Bus &bus, unsigned accuracy, ShtSample &out) {
        uint8_t data[6];
        const bool sht2 = model == ShtModel::Sht2;
        const bool sht4 = model == ShtModel::Sht4;
        int rc;
        if (sht2) {
            if ((rc = read(bus, 0xf3, 1, 85, data, 3)) || (rc = read(bus, 0xf5, 1, 85, data + 3, 3)))
                return rc;
        } else {
            static constexpr uint16_t cmd3[] = {0x2416, 0x240b, 0x2400};
            static constexpr uint8_t cmd4[] = {0xe0, 0xf6, 0xfd}, time3[] = {4, 6, 15}, time4[] = {2, 4, 10};
            const auto cmd = sht4 ? cmd4[accuracy] : model == ShtModel::ShtC ? 0x7866 : cmd3[accuracy];
            const unsigned duration = sht4 ? time4[accuracy] : model == ShtModel::ShtC ? 15 : time3[accuracy];
            if ((rc = read(bus, cmd, sht4 ? 1 : 2, duration, data, sizeof(data)))) return rc;
        }
        const uint8_t init = sht2 ? 0 : 0xff;
        if (crc(data, 2, init) != data[2] || crc(data + 3, 2, init) != data[5]) return -EBADMSG;
        if (sht2 && ((data[1] & 2) != 0 || (data[4] & 2) != 2)) return -EBADMSG;
        const uint16_t mask = sht2 ? 0xfffc : 0xffff;
        const uint16_t t = (uint16_t(data[0]) << 8 | data[1]) & mask;
        const uint16_t h = (uint16_t(data[3]) << 8 | data[4]) & mask;
        // Match SHTI2cSensor's float order, including each family's divisor.
        const float divisor = sht2 ? 65536.f : 65535.f;
        out.temperature = (sht2 ? -46.85f : -45.f) + (sht2 ? 175.72f : 175.f) * (t / divisor);
        out.humidity = (sht4 || sht2 ? -6.f : 0.f) + (sht4 || sht2 ? 125.f : 100.f) * (h / divisor);
        out.present = true;
        return 0;
    }
};
} // namespace mt::stock::telemetry::accessory
