#pragma once
#include <cstdint>
namespace Time {
inline uint64_t syntheticNowMs=0;
// AirTime consumes already unwrapped uptime seconds. The real upstream
// serviceMonotonic()/millis carry implementation is outside this fixture.
inline uint32_t getUptimeSecs() { return uint32_t(syntheticNowMs/1000); }
}
