// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/gps/GeoCoord.cpp, MESHTASTIC_TRIG_APPROX implementation.
#pragma once
#include <math.h>
namespace mt::stock::range {
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesPerRadian = 180 / Pi;
static double cosLatitudeApprox(double latRad)
{
    constexpr double c1 = 0.9999932946, c2 = -0.4999124376, c3 = 0.0414877472, c4 = -0.0012712095;
    double x2 = latRad * latRad;
    return c1 + x2 * (c2 + x2 * (c3 + c4 * x2));
}

/// Approximate distance in meters via equirectangular projection (not exact spherical trig).
/// <1% error to ~500km, degrading near the poles at long range (see test_geocoord_distance).
inline float latLongToMeter(double lat_a, double lng_a, double lat_b, double lng_b)
{
    // Don't do math if the points are the same
    if (lat_a == lat_b && lng_a == lng_b)
        return 0.0;

    double a1 = lat_a / DegreesPerRadian;
    double a2 = lng_a / DegreesPerRadian;
    double b1 = lat_b / DegreesPerRadian;
    double b2 = lng_b / DegreesPerRadian;

    double meanLat = (a1 + b1) / 2;
    double dLng = b2 - a2;
    // Wrap to [-Pi, Pi]: unlike cos()/sin(), a raw longitude difference doesn't handle points that
    // straddle the antimeridian (e.g. 179.9 and -179.9 are ~0.2 degrees apart, not ~360).
    if (dLng > Pi)
        dLng -= 2 * Pi;
    else if (dLng < -Pi)
        dLng += 2 * Pi;
    double x = dLng * cosLatitudeApprox(meanLat);
    double y = b1 - a1;
    double tt = sqrt(x * x + y * y);

    return (float)(6366000 * tt);
}
}
