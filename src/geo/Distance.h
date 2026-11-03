#pragma once

#include <QtMath>

#include <cmath>

namespace pl::geo {

// Great-circle distance between two points, in kilometres.
inline double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusKm = 6371.0;
    const double dLat = qDegreesToRadians(lat2 - lat1);
    const double dLon = qDegreesToRadians(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                    + std::cos(qDegreesToRadians(lat1)) * std::cos(qDegreesToRadians(lat2))
                          * std::sin(dLon / 2) * std::sin(dLon / 2);
    return kEarthRadiusKm * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

} // namespace pl::geo
