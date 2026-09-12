#include "geo/WebMercator.h"

#include <qmath.h>

namespace pl::geo {
namespace {
// Mercator's projection blows up at the poles; OSM (and everyone else) clamps
// to the latitude where the projected square world stops being square.
constexpr double kMaxLat = 85.05112878;

double clampLat(double lat)
{
    return qBound(-kMaxLat, lat, kMaxLat);
}
} // namespace

QPointF lonLatToWorldPixel(double lon, double lat, int zoom)
{
    const double worldSize = kTileSize * std::pow(2.0, zoom);
    const double x = (lon + 180.0) / 360.0 * worldSize;

    const double sinLat = std::sin(qDegreesToRadians(clampLat(lat)));
    const double y = (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * M_PI)) * worldSize;

    return {x, y};
}

QPointF worldPixelToLonLat(QPointF worldPixel, int zoom)
{
    const double worldSize = kTileSize * std::pow(2.0, zoom);
    const double lon = worldPixel.x() / worldSize * 360.0 - 180.0;

    const double n = M_PI - 2.0 * M_PI * worldPixel.y() / worldSize;
    const double lat = qRadiansToDegrees(std::atan(std::sinh(n)));

    return {lon, lat};
}

} // namespace pl::geo
