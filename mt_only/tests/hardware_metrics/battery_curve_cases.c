// SPDX-License-Identifier: MIT
//
// battery_curve_fixture.c
//
// Standalone C fixture for the ZephCore battery-curve public API
// (zephcore/helpers/battery_curve.h).
//
// Scope — valid curves only:
//   * default curve at zero / minimum / maximum / a known midpoint,
//   * a synthetic two-cell curve,
//   * monotonic, bounded output across the full 0..65535 mV range.
//
// This is a source-only proposal. It does not implement or modify the
// helper; it links against the existing public API and uses synthetic data
// only. No build, run, or test is claimed here.
//
// ZephCore donor 3c7a7c830ac4cdbd0df052f06995d3c7632b9b44;
// see zephcore/helpers/LICENSE.ZephCore for the full MIT notice.

/* The public API. The build must make zephcore/helpers/ an include path so
 * this resolves to zephcore/helpers/battery_curve.h. The header provides
 * the fixed-width integer types (uint16_t / uint8_t). */
#include "battery_curve.h"
#include <stdio.h>

/* Minimal assertion: report and count failures without aborting. */
static int battery_curve_fixture_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr,                                                \
                    "battery_curve_fixture: CHECK failed at line %d: %s\n",\
                    __LINE__, #cond);                                      \
            ++battery_curve_fixture_failures;                              \
        }                                                                  \
    } while (0)

/* Default curve: zero / minimum / maximum / a known midpoint. */
static void test_default_curve_endpoints(void)
{
    /* zero (and below-minimum) clamps to 0 */
    CHECK(battery_curve_lookup(&battery_curve_default, 0) == 0);
    /* minimum point is 0% */
    CHECK(battery_curve_lookup(&battery_curve_default, 3100) == 0);
    /* maximum point is 100% */
    CHECK(battery_curve_lookup(&battery_curve_default, 4190) == 100);
    /* known midpoint */
    CHECK(battery_curve_lookup(&battery_curve_default, 3720) == 50);
}

/* Synthetic two-cell curve (per-cell OCV points, descending). */
static void test_synthetic_two_cell_curve(void)
{
    /* Per-cell points {4200, 3600, 3000} = {100%, 50%, 0%}. With two cells
     * the total voltage is scaled by 2: 7800 mV total = 3900 mV/cell -> 75%;
     * 6600 mV total = 3300 mV/cell -> 25%. */
    const uint16_t points[] = {4200, 3600, 3000};
    const battery_curve_t two_cell = {points, 3, 2};

    CHECK(battery_curve_lookup(&two_cell, 7800) == 75);
    CHECK(battery_curve_lookup(&two_cell, 6600) == 25);
}

/* Monotonic, bounded output across the full 0..65535 mV range. */
static void test_monotonic_bounded_over_full_range(void)
{
    unsigned previous = 0;
    for (unsigned mv = 0; mv <= 65535u; ++mv) {
        const uint8_t percent =
            battery_curve_lookup(&battery_curve_default, (uint16_t)mv);
        CHECK(percent >= previous && percent <= 100);
        previous = percent;
    }
}

int main(void)
{
    test_default_curve_endpoints();
    test_synthetic_two_cell_curve();
    test_monotonic_bounded_over_full_range();

    if (battery_curve_fixture_failures == 0) {
        printf("PASS donor battery curve endpoints, synthetic two-cell and full-range monotonic bounds\n");
        return 0;
    }

    printf("battery_curve_fixture: %d check(s) failed\n",
           battery_curve_fixture_failures);
    return 1;
}
