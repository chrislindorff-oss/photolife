#include "net/INatParse.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <limits>

namespace pl::net::inat {
namespace {

std::optional<qint64> optId(const QJsonValue &v)
{
    if (v.isDouble())
        return qint64(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        if (ok)
            return n;
    }
    return std::nullopt;
}

} // namespace

QList<qint64> ancestryIds(const QString &ancestry)
{
    QList<qint64> ids;
    const QStringList parts =
        ancestry.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        bool ok = false;
        const qint64 n = p.toLongLong(&ok);
        if (ok)
            ids.append(n);
    }
    return ids;
}

pl::taxonomy::Taxon parseTaxon(const QJsonObject &obj)
{
    pl::taxonomy::Taxon t;
    t.inatId = optId(obj.value(QStringLiteral("id"))).value_or(0);
    t.rank = obj.value(QStringLiteral("rank")).toString();
    if (const QJsonValue rl = obj.value(QStringLiteral("rank_level")); rl.isDouble())
        t.rankLevel = int(rl.toDouble());
    t.name = obj.value(QStringLiteral("name")).toString();
    t.commonName = obj.value(QStringLiteral("preferred_common_name")).toString();
    t.ancestry = obj.value(QStringLiteral("ancestry")).toString();
    t.isActive = obj.value(QStringLiteral("is_active")).toBool(true);

    if (const auto pid = optId(obj.value(QStringLiteral("parent_id"))))
        t.parentInatId = pid;
    else if (const QList<qint64> chain = ancestryIds(t.ancestry); !chain.isEmpty())
        t.parentInatId = chain.last();

    if (!t.commonName.isEmpty())
        t.vernacular.append(t.commonName);

    const QJsonArray names = obj.value(QStringLiteral("names")).toArray();
    for (const QJsonValue &nv : names) {
        const QJsonObject n = nv.toObject();
        const QString name = n.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        const QString lexicon = n.value(QStringLiteral("lexicon")).toString().toLower();
        if (lexicon == QLatin1String("scientific-names")) {
            if (name != t.name)
                t.synonyms.append(name);
        } else if (!t.vernacular.contains(name)) {
            t.vernacular.append(name);
        }
    }

    t.synonyms.removeDuplicates();
    t.vernacular.removeDuplicates();
    return t;
}

pl::taxonomy::Place parsePlace(const QJsonObject &obj)
{
    pl::taxonomy::Place p;
    p.inatId = optId(obj.value(QStringLiteral("id"))).value_or(0);
    p.name = obj.value(QStringLiteral("name")).toString();
    p.displayName = obj.value(QStringLiteral("display_name")).toString();
    if (const QJsonValue al = obj.value(QStringLiteral("admin_level")); al.isDouble())
        p.adminLevel = int(al.toDouble());

    const QJsonObject box = obj.value(QStringLiteral("bounding_box_geojson")).toObject();
    const QJsonArray rings = box.value(QStringLiteral("coordinates")).toArray();
    if (!rings.isEmpty()) {
        double minLat = std::numeric_limits<double>::max();
        double minLng = std::numeric_limits<double>::max();
        double maxLat = std::numeric_limits<double>::lowest();
        double maxLng = std::numeric_limits<double>::lowest();
        bool any = false;
        for (const QJsonValue &pv : rings.first().toArray()) {
            const QJsonArray pt = pv.toArray();
            if (pt.size() < 2)
                continue;
            const double lng = pt.at(0).toDouble();
            const double lat = pt.at(1).toDouble();
            minLat = std::min(minLat, lat);
            maxLat = std::max(maxLat, lat);
            minLng = std::min(minLng, lng);
            maxLng = std::max(maxLng, lng);
            any = true;
        }
        if (any) {
            p.bboxSwLat = minLat;
            p.bboxSwLng = minLng;
            p.bboxNeLat = maxLat;
            p.bboxNeLng = maxLng;
        }
    }
    return p;
}

} // namespace pl::net::inat
