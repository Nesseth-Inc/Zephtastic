// SPDX-License-Identifier: GPL-3.0-only
#include "RadioPacketTiming.h"
#include "long_fast.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

// Host services for the unchanged stock methods. The caller controls the
// random draw so both implementations see the same value in each interval.
namespace stock {
using std::max;
inline uint32_t pow_of_2(uint8_t n) { return 1u << n; }
inline long map(long n,long lo,long hi,long a,long b) { return (n-lo)*(b-a)/(hi-lo)+a; }
uint32_t draw;
inline long random(long lo,long hi) { return lo + draw % (hi-lo); }
#define LOG_DEBUG_RADIO(...) ((void)0)

enum { meshtastic_Config_DeviceConfig_Role_ROUTER = 2 };
struct { struct { unsigned role; } device; } config;
struct meshtastic_MeshPacket { float rx_snr; };
struct Region { bool wideLora; };
Region subGHz{false};
Region *myRegion = &subGHz;

struct RadioInterface {
    #include "stock-cw.inc"
    uint8_t sf = 11;
    float bw = 250;
    uint32_t slotTimeMsec = 0;
    virtual uint8_t getCadSymbolCountSubGhz() const = 0;
    uint8_t getCadSymbolCountWideLora() const { std::abort(); }
    uint8_t getCadSymbolCount() const;
    uint32_t computeSlotTimeMsec();
    uint8_t getCWsize(float snr);
    bool shouldRebroadcastEarlyLikeRouter(meshtastic_MeshPacket *p);
    uint32_t getTxDelayMsecWeighted(meshtastic_MeshPacket *p);
    uint32_t getTxDelayMsecWeightedWorst(float snr);
};
struct SX126x : RadioInterface {
    #include "stock-sx126x-cad.inc"
};

#include "stock-radio.inc"
}

int main() {
    stock::SX126x radio;
    unsigned comparisons = 0;
    for (uint8_t sf = 5; sf <= 12; ++sf) {
        for (unsigned bw : {125000u,250000u,500000u}) {
            radio.sf = sf;
            radio.bw = float(bw) / 1000.0f;
            radio.slotTimeMsec = radio.computeSlotTimeMsec();
            uint32_t ztSlot;
            assert(mt::RadioSlot::resolve(bw,sf,ztSlot) && ztSlot == radio.slotTimeMsec);
            for (int snr = -20; snr <= 10; ++snr) {
                stock::meshtastic_MeshPacket packet{float(snr)};
                assert(radio.getTxDelayMsecWeightedWorst(packet.rx_snr) ==
                       mt::RebroadcastTiming::worstDelay(ztSlot,snr));
                for (unsigned role : {0u,2u,11u}) {
                    stock::config.device.role = role;
                    for (unsigned draw : {0u,1u,5u,60u,255u}) {
                        stock::draw = draw;
                        assert(radio.getTxDelayMsecWeighted(&packet) ==
                               mt::RebroadcastTiming::initialDelay(ztSlot,role == 2,snr,draw));
                        ++comparisons;
                    }
                }
            }
        }
    }
    stock::config.device.role = 11;
    stock::draw = 0;
    radio.sf = 11;
    radio.bw = 250;
    radio.slotTimeMsec = radio.computeSlotTimeMsec();
    stock::meshtastic_MeshPacket packet{3};
    const auto initial = radio.getTxDelayMsecWeighted(&packet);
    const auto duplicate = radio.getTxDelayMsecWeightedWorst(packet.rx_snr);
    const auto weakDuplicate = radio.getTxDelayMsecWeightedWorst(-20);
    assert(radio.slotTimeMsec == stock_long_fast::slotMs && initial == stock_long_fast::initialMs);
    assert(duplicate == stock_long_fast::duplicateMs && weakDuplicate == stock_long_fast::weakDuplicateMs);
    std::printf("{\"comparisons\":%u,\"slot_ms\":%u,\"initial_ms\":%u,"
                "\"duplicate_ms\":%u,\"weak_duplicate_ms\":%u}\n",
                comparisons,radio.slotTimeMsec,initial,duplicate,weakDuplicate);
}
