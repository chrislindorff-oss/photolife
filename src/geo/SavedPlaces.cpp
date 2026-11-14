#include "geo/SavedPlaces.h"

#include <algorithm>

namespace pl::geo {

QList<SavedPlace> parseSavedPlaces(const QStringList &stored)
{
    QList<SavedPlace> places;
    for (const QString &line : stored) {
        const QStringList f = line.split(QLatin1Char('\t'));
        if (f.size() != 5 || f.at(0).trimmed().isEmpty())
            continue;
        bool ok[4] = {};
        SavedPlace p;
        p.name = f.at(0).trimmed();
        p.box.swLat = f.at(1).toDouble(&ok[0]);
        p.box.swLng = f.at(2).toDouble(&ok[1]);
        p.box.neLat = f.at(3).toDouble(&ok[2]);
        p.box.neLng = f.at(4).toDouble(&ok[3]);
        if (ok[0] && ok[1] && ok[2] && ok[3])
            places << p;
    }
    return places;
}

QStringList formatSavedPlaces(const QList<SavedPlace> &places)
{
    QStringList out;
    for (const SavedPlace &p : places)
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5")
                   .arg(QString(p.name).remove(QLatin1Char('\t')).trimmed())
                   .arg(p.box.swLat, 0, 'f', 6)
                   .arg(p.box.swLng, 0, 'f', 6)
                   .arg(p.box.neLat, 0, 'f', 6)
                   .arg(p.box.neLng, 0, 'f', 6);
    return out;
}

std::optional<SavedPlace> findSavedPlace(const QList<SavedPlace> &places, const QString &name)
{
    for (const SavedPlace &p : places) {
        if (p.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return p;
    }
    return std::nullopt;
}

QList<SavedPlace> withSavedPlace(QList<SavedPlace> places, const SavedPlace &place)
{
    places.removeIf([&](const SavedPlace &p) { return p.name.compare(place.name, Qt::CaseInsensitive) == 0; });
    places << place;
    std::sort(places.begin(), places.end(), [](const SavedPlace &a, const SavedPlace &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return places;
}

PlaceQuery extractPlace(const QString &search, const QList<SavedPlace> &places)
{
    PlaceQuery out;
    const QString text = search.simplified();
    const QString key = QStringLiteral("place:");
    const qsizetype at = text.indexOf(key, 0, Qt::CaseInsensitive);
    if (at < 0 || (at > 0 && text.at(at - 1) != QLatin1Char(' '))) {
        out.rest = text;
        return out;
    }
    const QString before = text.left(at).trimmed();
    QString after = text.mid(at + key.size());
    QString name, tail;
    if (after.startsWith(QLatin1Char('"'))) {
        const qsizetype close = after.indexOf(QLatin1Char('"'), 1);
        name = close < 0 ? after.mid(1) : after.mid(1, close - 1);
        tail = close < 0 ? QString() : after.mid(close + 1);
        if (auto p = findSavedPlace(places, name))
            out.place = p;
    } else {
        // The longest saved name it starts with, ending at a word's end.
        for (const SavedPlace &p : places) {
            if (after.startsWith(p.name, Qt::CaseInsensitive)
                && (after.size() == p.name.size() || after.at(p.name.size()) == QLatin1Char(' '))
                && (!out.place || p.name.size() > out.place->name.size()))
                out.place = p;
        }
        if (out.place) {
            name = out.place->name;
            tail = after.mid(name.size());
        } else {
            name = after.section(QLatin1Char(' '), 0, 0);
            tail = after.section(QLatin1Char(' '), 1);
        }
    }
    if (!out.place)
        out.unknown = name.trimmed();
    out.rest = (before + QLatin1Char(' ') + tail).simplified();
    return out;
}

QString boxClause(const GeoBox &box, const QString &latColumn, const QString &lonColumn)
{
    const auto num = [](double v) { return QString::number(v, 'f', 7); };
    const QString lat = QStringLiteral("%1 BETWEEN %2 AND %3")
                            .arg(latColumn, num(std::min(box.swLat, box.neLat)), num(std::max(box.swLat, box.neLat)));
    const QString lon = box.swLng <= box.neLng
                            ? QStringLiteral("%1 BETWEEN %2 AND %3").arg(lonColumn, num(box.swLng), num(box.neLng))
                            : QStringLiteral("(%1 >= %2 OR %1 <= %3)").arg(lonColumn, num(box.swLng), num(box.neLng));
    return QStringLiteral("(%1 IS NOT NULL AND %2 IS NOT NULL AND %3 AND %4)").arg(latColumn, lonColumn, lat, lon);
}

} // namespace pl::geo
