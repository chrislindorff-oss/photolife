#include "inat/DuplicateFinder.h"

#include "geo/Distance.h"
#include "net/INatParse.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace pl::inat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("pl::inat::DuplicateFinder", text);
}

// Wall-clock seconds from `a` to `b`, whatever zone either is held in.
qint64 clockSecondsBetween(const QDateTime &a, const QDateTime &b)
{
    return a.date().daysTo(b.date()) * 86400 + a.time().secsTo(b.time());
}

QString apart(qint64 seconds)
{
    if (seconds < 60)
        return tr("the same minute");
    if (seconds < 3600)
        return tr("%1 min apart").arg(seconds / 60);
    const qint64 hours = (seconds + 1800) / 3600;
    return hours == 1 ? tr("1 hour apart") : tr("%1 hours apart").arg(hours);
}

QString away(double km)
{
    if (km < 0.05)
        return tr("the same place");
    return tr("%1 m away").arg(qRound(km * 1000 / 10) * 10);
}

} // namespace

ExistingObservation ExistingObservation::fromInat(const taxonomy::Observation &o)
{
    ExistingObservation e;
    e.id = o.id;
    e.taxonId = o.taxonInatId;
    e.taxonAncestorIds = o.taxonAncestorIds;
    e.taxonName = o.taxonCommonName.isEmpty() ? o.taxonName
                                              : QStringLiteral("%1 · %2").arg(o.taxonCommonName, o.taxonName);
    e.date = QDate::fromString(o.observedOn.left(10), Qt::ISODate);
    e.time = o.timeObserved;
    if (!e.date.isValid() && e.time.isValid())
        e.date = e.time.date();
    e.latitude = o.latitude;
    e.longitude = o.longitude;
    e.placeGuess = o.placeGuess;
    for (const taxonomy::ObservationPhoto &p : o.photos)
        if (!p.previewUrl.isEmpty())
            e.photoUrls << p.previewUrl;
    return e;
}

namespace DuplicateFinder {

QList<DuplicateHit> find(const ProposedObservation &proposed, const QList<ExistingObservation> &existing)
{
    QList<DuplicateHit> hits;
    const QDateTime at = proposed.observedAt();
    if (!at.isValid())
        return hits;
    const bool hasTime = proposed.hasTime();
    const std::optional<QPointF> where = proposed.where();
    const qint64 taxon = proposed.taxon.inatId;
    const QList<qint64> ancestors = net::inat::ancestryIds(proposed.taxon.ancestry);
    QSet<qint64> own;   // observations these photos are part of already
    if (proposed.observationId > 0)
        own.insert(proposed.observationId);
    for (const UploadPhoto &p : proposed.photos)
        if (p.observationId > 0)
            own.insert(p.observationId);

    for (const ExistingObservation &e : existing) {
        if (own.contains(e.id) || !e.date.isValid())
            continue;

        QString species;
        if (e.taxonId <= 0)
            species = tr("not identified there");
        else if (taxon <= 0)
            species = QString();   // nothing to compare yet
        else if (e.taxonId == taxon)
            species = tr("same species");
        else if (e.taxonAncestorIds.contains(taxon) || ancestors.contains(e.taxonId))
            species = tr("a related taxon");
        else
            continue;

        DuplicateHit hit;
        QString when;
        if (hasTime && e.time.isValid()) {
            hit.secondsApart = std::abs(clockSecondsBetween(at, e.time));
            if (hit.secondsApart > kTimeWindowSeconds)
                continue;
            when = apart(hit.secondsApart);
        } else {
            if (e.date != at.date())
                continue;
            when = tr("same day");
        }

        QString place;
        if (where && e.latitude && e.longitude) {
            hit.km = geo::haversineKm(where->y(), where->x(), *e.latitude, *e.longitude);
            if (hit.km > kRadiusKm)
                continue;
            place = away(hit.km);
        } else {
            place = tr("no location to compare");
        }

        hit.existing = e;
        QStringList reasons;
        if (!species.isEmpty())
            reasons << species;
        reasons << when << place;
        hit.reasons = reasons.join(QStringLiteral(", "));
        hits << hit;
    }
    std::stable_sort(hits.begin(), hits.end(), [](const DuplicateHit &a, const DuplicateHit &b) {
        // Unknown times sort as half a day apart, unknown places as at the edge.
        const qint64 ta = a.secondsApart < 0 ? kTimeWindowSeconds / 2 : a.secondsApart;
        const qint64 tb = b.secondsApart < 0 ? kTimeWindowSeconds / 2 : b.secondsApart;
        if (ta != tb)
            return ta < tb;
        return (a.km < 0 ? kRadiusKm : a.km) < (b.km < 0 ? kRadiusKm : b.km);
    });
    return hits;
}

QList<QPair<QDate, QDate>> dateRanges(const QList<ProposedObservation> &proposed)
{
    QList<QDate> days;
    for (const ProposedObservation &p : proposed)
        if (const QDateTime at = p.observedAt(); at.isValid())
            days << at.date();
    std::sort(days.begin(), days.end());
    QList<QPair<QDate, QDate>> ranges;
    for (const QDate &d : std::as_const(days)) {
        const QDate from = d.addDays(-1), to = d.addDays(1);
        if (!ranges.isEmpty() && from <= ranges.last().second.addDays(1))
            ranges.last().second = std::max(ranges.last().second, to);
        else
            ranges.append({from, to});
    }
    return ranges;
}

} // namespace DuplicateFinder

} // namespace pl::inat
