#pragma once

#include <QJsonObject>
#include <QString>

namespace pl::net::geocode {

// Reduces a Nominatim /reverse response to a short "Town, State, Country"
// locality string. Empty if Nominatim had no address for the coordinate
// (e.g. open ocean) -- not an error case, just nothing to show.
QString formatLocality(const QJsonObject &response);

} // namespace pl::net::geocode
