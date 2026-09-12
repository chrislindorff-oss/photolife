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

    const QString town =
        firstOf(address, {"city", "town", "village", "hamlet", "suburb", "municipality"});
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

} // namespace pl::net::geocode
