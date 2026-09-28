#pragma once

#include <QList>
#include <QPointF>
#include <QString>

#include <optional>

namespace pl::geo {

// A region's outline, e.g. an iNaturalist place's geometry_geojson: every ring
// of every polygon (outer boundaries and holes alike) as (lon, lat) points.
// A point is inside when it lies within an odd number of rings, which is
// right for a MultiPolygon's separate parts and their holes.
//
// Rings are unwrapped across the antimeridian as they are parsed -- a ring
// running from 179° to -179° continues to 181° instead -- so they draw as one
// shape and can be tested with plain arithmetic.
struct GeoShape
{
    QList<QList<QPointF>> rings;
};

// A GeoJSON Polygon or MultiPolygon geometry (the object itself, as JSON
// text). nullopt for anything else, or text that isn't one.
std::optional<GeoShape> parseGeoJsonShape(const QString &geoJson);

// True when (lon, lat) lies inside the shape (see GeoShape).
bool shapeContains(const GeoShape &shape, double lon, double lat);

} // namespace pl::geo
