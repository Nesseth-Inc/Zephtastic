// SPDX-License-Identifier: GPL-3.0-only
// Shared pure packet timing policy; no application, clock or radio dependency.
#pragma once
#include <stdint.h>
#include <math.h>


namespace mt { namespace RadioTiming {
struct Deadline { uint32_t driverMs=0,hostMs=0; };
// Mirrors the pinned SX126x driver's internal timeout. This helper does not
// configure that timeout; hostMs adds a local completion margin.
inline bool resolve(uint32_t airMs,Deadline &out) {
  if(!airMs)return false;
  uint64_t driver=uint64_t(airMs)+airMs/4u+500u;
  if(driver<10000u)driver=10000u;
  if(driver>262143u)driver=262143u;
  if(airMs>=driver)return false;
  out={uint32_t(driver),uint32_t(driver)+1000u};return true;
}
} }

namespace mt { namespace PacketLifetime {
struct Bounds {uint32_t queueMs=0,pendingMs=0,historyMs=0;};
// Local bounded lifetime policy. Callers capture bounds at admission; this
// does not change nonce/session, opaque-frame or route-cache retention.
inline bool resolve(uint32_t hostMs,uint32_t retransmissionMs,Bounds &out) {
  if(!hostMs||!retransmissionMs)return false;
  const uint64_t cycle=uint64_t(hostMs)+retransmissionMs+1000u;
  const uint64_t pending=3*cycle>90000u?3*cycle:90000u;
  const uint64_t queue=uint64_t(hostMs)+10000u>30000u?uint64_t(hostMs)+10000u:30000u;
  const uint64_t history=pending+queue+1000u>600000u?pending+queue+1000u:600000u;
  if(pending>=0x80000000u||queue>=0x80000000u||history>=0x80000000u)return false;
  out={uint32_t(queue),uint32_t(pending),uint32_t(history)};return true;
}
} }

namespace mt { namespace RadioSlot {
// SX126x runs four CAD symbols. Stock adds half a symbol and 7.6 ms of
// turnaround/processing, then truncates. SF12/BW7.8 needs 2370 ms.
static constexpr uint32_t MaxSlotMs=2371;
inline bool resolve(uint32_t bandwidthHz,unsigned sf,uint32_t &out) {
  if(!bandwidthHz||sf<5||sf>12)return false;
  const float bwKHz=float(bandwidthHz)/1000.0f;
  const float symbolTime=float(1u<<sf)/bwKHz;
  const float overhead=0.2+0.4+7;
  const double slot=4.5*symbolTime+overhead;
  if(!isfinite(slot)||slot<1||slot>MaxSlotMs)return false;
  out=uint32_t(slot);return true;
}
} }

// Meshtastic 2.8.2 RadioInterface::getCWsize, getTxDelayMsecWeighted and
// getTxDelayMsecWeightedWorst. The radio adapter supplies integer dB and
// the random draw. Bound SNR to the supported range before calculating CW.
namespace mt { namespace RebroadcastTiming {
constexpr uint8_t CWmin = 3;
constexpr uint8_t CWmax = 8;

inline uint8_t contentionWindow(int8_t snr) {
    const int32_t bounded = snr < -20 ? -20 : snr > 10 ? 10 : snr;
    return uint8_t(CWmin + (bounded + 20) * (CWmax - CWmin) / 30);
}

inline uint32_t initialDelay(uint32_t slotMs, bool earlyRouter, int8_t snr, uint32_t draw) {
    const uint8_t cw = contentionWindow(snr);
    if (earlyRouter) return (draw % (2u * cw)) * slotMs;
    return (2u * CWmax + draw % (1u << cw)) * slotMs;
}

inline uint32_t worstDelay(uint32_t slotMs, int8_t snr) {
    return (2u * CWmax + (1u << contentionWindow(snr))) * slotMs;
}
} }
