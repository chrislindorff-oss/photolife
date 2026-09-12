#pragma once

namespace pl::geo {

// A lat/lon bounding box, e.g. an iNaturalist place's extent. Assumes it does
// not straddle the antimeridian, consistent with how net::inat::parsePlace()
// computes these boxes.
struct GeoBox
{
    double swLat = 0.0;
    double swLng = 0.0;
    double neLat = 0.0;
    double neLng = 0.0;
};

// True when (lon, lat) falls within box, inclusive of its edges.
bool boxContains(const GeoBox &box, double lon, double lat);

} // namespace pl::geo
