#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace pl::net::geocode {

// Reduces a Nominatim /reverse response to a short "Town, State, Country"
// locality string. Empty if Nominatim had no address for the coordinate
// (e.g. open ocean) -- not an error case, just nothing to show.
QString formatLocality(const QJsonObject &response);

// One place found by a Nominatim /search.
struct Place
{
    QString name;      // its full display name
    double latitude = 0;
    double longitude = 0;
};
// The places in a Nominatim /search response (a JSON array), best first.
QList<Place> parseSearch(const QJsonArray &response);

} // namespace pl::net::geocode
