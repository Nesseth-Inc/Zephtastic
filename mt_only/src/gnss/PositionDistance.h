// SPDX-License-Identifier: GPL-3.0-only
// Literal pinned Meshtastic 8e6a88d trig-approx functions; namespace/signature only retargeted.
#pragma once
#include <math.h>
namespace mt::PositionDistance {
static constexpr double PI=3.1415926535897932384626433832795;
static constexpr double DEG_CONVERT=(180/PI);
inline double cosLatitudeApprox(double latRad)
{
    constexpr double c1 = 0.9999932946, c2 = -0.4999124376, c3 = 0.0414877472, c4 = -0.0012712095;
    double x2 = latRad * latRad;
    return c1 + x2 * (c2 + x2 * (c3 + c4 * x2));
}
inline float meters(double lat_a, double lng_a, double lat_b, double lng_b)
{
    // Don't do math if the points are the same
    if (lat_a == lat_b && lng_a == lng_b)
        return 0.0;

    double a1 = lat_a / DEG_CONVERT;
    double a2 = lng_a / DEG_CONVERT;
    double b1 = lat_b / DEG_CONVERT;
    double b2 = lng_b / DEG_CONVERT;

    double meanLat = (a1 + b1) / 2;
    double dLng = b2 - a2;
    // Wrap to [-PI, PI]: unlike cos()/sin(), a raw longitude difference doesn't handle points that
    // straddle the antimeridian (e.g. 179.9 and -179.9 are ~0.2 degrees apart, not ~360).
    if (dLng > PI)
        dLng -= 2 * PI;
    else if (dLng < -PI)
        dLng += 2 * PI;
    double x = dLng * cosLatitudeApprox(meanLat);
    double y = b1 - a1;
    double tt = sqrt(x * x + y * y);

    return (float)(6366000 * tt);
}
}
