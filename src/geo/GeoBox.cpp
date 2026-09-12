#include "geo/GeoBox.h"

namespace pl::geo {

bool boxContains(const GeoBox &box, double lon, double lat)
{
    return lat >= box.swLat && lat <= box.neLat && lon >= box.swLng && lon <= box.neLng;
}

} // namespace pl::geo
