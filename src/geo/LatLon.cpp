#include "geo/LatLon.h"

#include <QRegularExpression>

namespace pl::geo {

QString formatLatLon(double latitude, double longitude)
{
    return QStringLiteral("%1, %2").arg(latitude, 0, 'f', 6).arg(longitude, 0, 'f', 6);
}

std::optional<QPointF> parseLatLon(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*([-+]?\d+(?:\.\d+)?)\s*(?:[,;]\s*|\s+)([-+]?\d+(?:\.\d+)?)\s*$)"));
    const QRegularExpressionMatch m = pattern.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    const double lat = m.captured(1).toDouble();
    const double lon = m.captured(2).toDouble();
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180)
        return std::nullopt;
    return QPointF(lon, lat);
}

} // namespace pl::geo
