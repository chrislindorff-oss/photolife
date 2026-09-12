#pragma once

#include <QPointF>

namespace pl::geo {

// Standard Web Mercator (EPSG:3857) tile projection, as used by OpenStreetMap
// and virtually every other slippy-map tile server: at zoom level `zoom` the
// whole world is TileSize * 2^zoom pixels square, longitude maps linearly onto
// the x axis, and latitude maps onto y via the Mercator formula below.
constexpr int kTileSize = 256;

// Longitude/latitude (degrees) -> pixel position in the world image at `zoom`.
QPointF lonLatToWorldPixel(double lon, double lat, int zoom);

// The inverse of lonLatToWorldPixel.
QPointF worldPixelToLonLat(QPointF worldPixel, int zoom);

} // namespace pl::geo
