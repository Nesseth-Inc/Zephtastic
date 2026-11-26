// SPDX-License-Identifier: GPL-3.0-only
// gnss_measurement_units_tests.cpp
// Source-only fixture for zt::gnss measurement-unit conversions.
// No framework or OS dependencies; plain assert() based.
#include "gnss/GnssMeasurementUnits.h"

#include <assert.h>
#include <stdint.h>
#include <limits.h>

int main()
{
    // millimetersToMeters: signed division truncated toward zero.
    assert(zt::gnss::millimetersToMeters(INT32_MIN) == -2147483);
    assert(zt::gnss::millimetersToMeters(INT32_MAX) == 2147483);
    assert(zt::gnss::millimetersToMeters(-1000) == -1);
    assert(zt::gnss::millimetersToMeters(-999) == 0);
    assert(zt::gnss::millimetersToMeters(0) == 0);
    assert(zt::gnss::millimetersToMeters(999) == 0);
    assert(zt::gnss::millimetersToMeters(1000) == 1);

    // millimetersPerSecondToKmh: floor of uint64_t(v) * 36 / 10000.
    assert(zt::gnss::millimetersPerSecondToKmh(0) == 0);
    assert(zt::gnss::millimetersPerSecondToKmh(9999) == 35);
    assert(zt::gnss::millimetersPerSecondToKmh(10000) == 36);
    assert(zt::gnss::millimetersPerSecondToKmh(27778) == 100);
    assert(zt::gnss::millimetersPerSecondToKmh(UINT32_MAX) == 15461882);

    // Bounded monotonicity: converted speed never decreases over 0..100000.
    const uint64_t bound = 100000ULL;
    uint32_t prev = 0;
    for (uint64_t i = 0; i <= bound; ++i)
    {
        const uint32_t cur = zt::gnss::millimetersPerSecondToKmh((uint32_t)i);
        assert(cur >= prev);
        prev = cur;
    }

    return 0;
}
