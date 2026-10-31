#include "net/GeocodeParse.h"

#include <QStringList>

namespace pl::net::geocode {
namespace {

QString firstOf(const QJsonObject &address, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString value = address.value(QLatin1String(key)).toString();
        if (!value.isEmpty())
            return value;
    }
    return {};
}

} // namespace

QString formatLocality(const QJsonObject &response)
{
    const QJsonObject address = response.value(QStringLiteral("address")).toObject();
    if (address.isEmpty())
        return {};

    // Away from towns -- national parks, coast and bush -- OpenStreetMap
    // still names the gazetted locality (Australia's suburbs and localities):
    // as "locality", or as "city_district" for an admin boundary
    // ("Cooloola", "Murray-Sunset", "Cape Bridgewater").
    const QString town =
        firstOf(address, {"city", "town", "village", "hamlet", "suburb", "municipality", "locality",
                          "city_district", "neighbourhood", "quarter", "isolated_dwelling"});
    const QString state = firstOf(address, {"state", "state_district", "region"});
    const QString country = firstOf(address, {"country"});

    QStringList parts;
    if (!town.isEmpty())
        parts << town;
    if (!state.isEmpty())
        parts << state;
    if (!country.isEmpty())
        parts << country;
    return parts.join(QStringLiteral(", "));
}

QList<Place> parseSearch(const QJsonArray &response)
{
    QList<Place> out;
    for (const QJsonValue &v : response) {
        const QJsonObject o = v.toObject();
        bool latOk = false, lonOk = false;
        Place p;
        // Nominatim gives the coordinates as strings.
        p.latitude = o.value(QStringLiteral("lat")).toString().toDouble(&latOk);
        p.longitude = o.value(QStringLiteral("lon")).toString().toDouble(&lonOk);
        p.name = o.value(QStringLiteral("display_name")).toString();
        if (latOk && lonOk && !p.name.isEmpty())
            out << p;
    }
    return out;
}

} // namespace pl::net::geocode
