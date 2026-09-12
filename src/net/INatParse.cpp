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

    // default_photo carries several sized URLs; prefer a mid-size one for the
    // Reference Photos grid, falling back to whatever is present.
    const QJsonObject photo = obj.value(QStringLiteral("default_photo")).toObject();
    if (!photo.isEmpty()) {
        for (const char *key : {"medium_url", "small_url", "square_url", "url"}) {
            const QString u = photo.value(QLatin1String(key)).toString();
            if (!u.isEmpty()) {
                t.photoUrl = u;
                break;
            }
        }
        t.photoAttribution = photo.value(QStringLiteral("attribution")).toString();
    }

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

pl::taxonomy::Observation parseObservation(const QJsonObject &obj)
{
    pl::taxonomy::Observation o;
    o.id = optId(obj.value(QStringLiteral("id"))).value_or(0);
    o.observedOn = obj.value(QStringLiteral("observed_on")).toString();
    o.placeGuess = obj.value(QStringLiteral("place_guess")).toString();

    const QJsonObject taxon = obj.value(QStringLiteral("taxon")).toObject();
    if (!taxon.isEmpty())
        o.taxonInatId = optId(taxon.value(QStringLiteral("id"))).value_or(0);

    // Same [lng, lat] GeoJSON point convention already used for a place's
    // bounding box above. Absent (geoprivacy-obscured with no public
    // fallback, or simply not recorded) rather than (0, 0) when missing.
    const QJsonObject geojson = obj.value(QStringLiteral("geojson")).toObject();
    const QJsonArray point = geojson.value(QStringLiteral("coordinates")).toArray();
    if (point.size() == 2) {
        o.longitude = point.at(0).toDouble();
        o.latitude = point.at(1).toDouble();
    }

    const QJsonArray photos = obj.value(QStringLiteral("photos")).toArray();
    for (const QJsonValue &photoValue : photos) {
        const QJsonObject photoObj = photoValue.toObject();
        pl::taxonomy::ObservationPhoto photo;
        photo.id = optId(photoObj.value(QStringLiteral("id"))).value_or(0);

        // Prefer an explicit sized field if the response included one (some
        // Photo shapes do, like default_photo's medium_url/etc.); the base
        // "url" iNat always includes is normally the smallest ("square").
        const QString base = photoObj.value(QStringLiteral("url")).toString();
        auto explicitUrl = [&](std::initializer_list<const char *> keys) -> QString {
            for (const char *key : keys) {
                const QString u = photoObj.value(QLatin1String(key)).toString();
                if (!u.isEmpty())
                    return u;
            }
            return {};
        };
        // Derives a different size from the default "url" via iNat's
        // size-suffix URL convention (".../photos/<id>/square.jpg" etc.).
        auto derivedUrl = [&](const char *targetSize) -> QString {
            for (const char *size : {"square", "small", "medium", "large", "original"}) {
                if (base.contains(QLatin1String(size)))
                    return QString(base).replace(QLatin1String(size),
                                                 QLatin1String(targetSize));
            }
            return {};
        };

        photo.previewUrl = explicitUrl({"small_url", "square_url", "medium_url"});
        if (photo.previewUrl.isEmpty())
            photo.previewUrl = !base.isEmpty() ? base : derivedUrl("small");

        photo.downloadUrl = explicitUrl({"original_url", "large_url", "medium_url"});
        if (photo.downloadUrl.isEmpty())
            photo.downloadUrl = derivedUrl("original");
        if (photo.downloadUrl.isEmpty())
            photo.downloadUrl = base;

        if (!photo.previewUrl.isEmpty() || !photo.downloadUrl.isEmpty())
            o.photos.append(photo);
    }
    return o;
}

} // namespace pl::net::inat
