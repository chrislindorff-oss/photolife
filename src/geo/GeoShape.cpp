#include "geo/GeoShape.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace pl::geo {
namespace {

// One GeoJSON linear ring, unwrapped across the antimeridian.
QList<QPointF> parseRing(const QJsonArray &coords)
{
    QList<QPointF> ring;
    for (const QJsonValue &v : coords) {
        const QJsonArray pt = v.toArray();
        if (pt.size() < 2)
            continue;
        double lon = pt.at(0).toDouble();
        const double lat = pt.at(1).toDouble();
        if (!ring.isEmpty()) {
            const double prev = ring.last().x();
            while (lon - prev > 180.0)
                lon -= 360.0;
            while (prev - lon > 180.0)
                lon += 360.0;
        }
        ring.append(QPointF(lon, lat));
    }
    return ring;
}

void addPolygon(GeoShape &shape, const QJsonArray &polygon)
{
    for (const QJsonValue &r : polygon) {
        QList<QPointF> ring = parseRing(r.toArray());
        if (ring.size() >= 3)
            shape.rings.append(std::move(ring));
    }
}

// Even-odd ray cast against one ring.
bool ringContains(const QList<QPointF> &ring, double lon, double lat)
{
    bool inside = false;
    for (qsizetype i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const QPointF &a = ring.at(i);
        const QPointF &b = ring.at(j);
        if ((a.y() > lat) != (b.y() > lat)
            && lon < (b.x() - a.x()) * (lat - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

} // namespace

std::optional<GeoShape> parseGeoJsonShape(const QString &geoJson)
{
    const QJsonObject obj = QJsonDocument::fromJson(geoJson.toUtf8()).object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QJsonArray coords = obj.value(QStringLiteral("coordinates")).toArray();

    GeoShape shape;
    if (type == QLatin1String("Polygon")) {
        addPolygon(shape, coords);
    } else if (type == QLatin1String("MultiPolygon")) {
        for (const QJsonValue &p : coords)
            addPolygon(shape, p.toArray());
    }
    if (shape.rings.isEmpty())
        return std::nullopt;
    return shape;
}

bool shapeContains(const GeoShape &shape, double lon, double lat)
{
    // An unwrapped ring may sit partly beyond ±180°, so try the point there too.
    for (const double l : {lon, lon + 360.0, lon - 360.0}) {
        bool inside = false;
        for (const QList<QPointF> &ring : shape.rings) {
            if (ringContains(ring, l, lat))
                inside = !inside;
        }
        if (inside)
            return true;
    }
    return false;
}

} // namespace pl::geo
