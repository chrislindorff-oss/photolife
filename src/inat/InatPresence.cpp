#include "inat/InatPresence.h"

#include "db/Database.h"
#include "geo/Distance.h"
#include "inat/ObservationUploader.h"
#include "net/INatParse.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pl::inat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("pl::inat::InatPresence", text);
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
    if (km < 10)
        return tr("%1 m away").arg(qRound(km * 1000 / 10) * 10);
    return tr("%1 km away").arg(qRound(km));
}

int rank(PresenceConfidence c)
{
    return int(c);
}

} // namespace

QString confidenceKey(PresenceConfidence confidence)
{
    switch (confidence) {
    case PresenceConfidence::High:
        return QStringLiteral("high");
    case PresenceConfidence::Medium:
        return QStringLiteral("medium");
    case PresenceConfidence::Low:
        return QStringLiteral("low");
    case PresenceConfidence::None:
        break;
    }
    return {};
}

PresenceConfidence confidenceFromKey(const QString &key)
{
    if (key == QLatin1String("high"))
        return PresenceConfidence::High;
    if (key == QLatin1String("medium"))
        return PresenceConfidence::Medium;
    if (key == QLatin1String("low"))
        return PresenceConfidence::Low;
    return PresenceConfidence::None;
}

PresenceMatcher::PresenceMatcher(QList<ExistingObservation> observations)
    : m_observations(std::move(observations))
{
    for (int i = 0; i < m_observations.size(); ++i) {
        const ExistingObservation &o = m_observations.at(i);
        if (o.date.isValid())
            m_byDay[o.date] << i;
        if (o.taxonId > 0)
            m_byTaxon[o.taxonId] << i;
    }
}

PresenceHit PresenceMatcher::compare(const PresencePhoto &photo, const ExistingObservation &obs) const
{
    PresenceHit hit;
    hit.observationId = obs.id;

    // The species first: the same, related (a genus or a subspecies either
    // way), or not identified there.
    enum class Taxon { Same, Related, Unidentified };
    Taxon taxon;
    if (obs.taxonId <= 0)
        taxon = Taxon::Unidentified;
    else if (photo.taxonInatId > 0 && obs.taxonId == photo.taxonInatId)
        taxon = Taxon::Same;
    else if (photo.taxonInatId > 0
             && (obs.taxonAncestorIds.contains(photo.taxonInatId) || photo.taxonAncestorIds.contains(obs.taxonId)))
        taxon = Taxon::Related;
    else
        return hit;

    const bool located = photo.latitude && photo.longitude && obs.latitude && obs.longitude;
    if (located)
        hit.km = geo::haversineKm(*photo.latitude, *photo.longitude, *obs.latitude, *obs.longitude);
    const QString species = taxon == Taxon::Same      ? tr("same species")
                            : taxon == Taxon::Related ? tr("a related taxon")
                                                      : tr("not identified there");

    // No date on the photo: only the same species at the same spot, any date.
    if (!photo.day.isValid()) {
        if (taxon != Taxon::Same || !located || hit.km > kNearKm)
            return hit;
        hit.confidence = PresenceConfidence::Low;
        hit.reasons = QStringList{species, tr("photo has no date"), away(hit.km)}.join(QStringLiteral(", "));
        return hit;
    }

    // When: both timed, within 12 hours (close: an hour); otherwise the same day.
    bool close = false;
    QString when;
    if (photo.taken.isValid() && (obs.time.isValid() || obs.timeEntered.isValid())) {
        // iNaturalist's time, or the time as entered (a camera's clock sent
        // with the wrong zone): whichever is closer.
        hit.secondsApart = std::numeric_limits<qint64>::max();
        for (const QDateTime &t : {obs.time, obs.timeEntered})
            if (t.isValid())
                hit.secondsApart = std::min(hit.secondsApart, std::abs(clockSecondsBetween(photo.taken, t)));
        if (hit.secondsApart > kWindowSeconds)
            return hit;
        close = hit.secondsApart <= kCloseSeconds;
        when = apart(hit.secondsApart);
    } else {
        if (!obs.date.isValid() || obs.date != photo.day)
            return hit;
        close = !photo.taken.isValid();   // a photo known only by its day: the day is all there is
        when = tr("same day");
    }

    if (located) {
        if (hit.km > kRadiusKm)
            return hit;
        const bool near = hit.km <= kNearKm;
        if (taxon == Taxon::Same && close && near)
            hit.confidence = PresenceConfidence::High;
        else if (taxon == Taxon::Unidentified && !(close && near))
            return hit;   // nothing but time and place to go on: they must be close
        else
            hit.confidence = PresenceConfidence::Medium;
    } else {
        // A location missing: the time and the species carry it.
        if (taxon == Taxon::Same)
            hit.confidence = PresenceConfidence::Medium;
        else if (close)
            hit.confidence = PresenceConfidence::Low;
        else
            return hit;
    }
    hit.reasons = QStringList{species, when, located ? away(hit.km) : tr("no location to compare")}
                      .join(QStringLiteral(", "));
    return hit;
}

PresenceHit PresenceMatcher::best(const PresencePhoto &photo) const
{
    QList<int> candidates;
    if (photo.day.isValid()) {
        for (int d = -1; d <= 1; ++d)
            candidates += m_byDay.value(photo.day.addDays(d));
    } else {
        candidates = m_byTaxon.value(photo.taxonInatId);
    }
    PresenceHit best;
    for (int i : std::as_const(candidates)) {
        const PresenceHit hit = compare(photo, m_observations.at(i));
        if (!hit.found())
            continue;
        if (!best.found() || rank(hit.confidence) > rank(best.confidence))
            best = hit;
        else if (hit.confidence == best.confidence) {
            // Closer in time, then in place.
            const qint64 ta = hit.secondsApart < 0 ? kWindowSeconds : hit.secondsApart;
            const qint64 tb = best.secondsApart < 0 ? kWindowSeconds : best.secondsApart;
            const double ka = hit.km < 0 ? kRadiusKm : hit.km;
            const double kb = best.km < 0 ? kRadiusKm : best.km;
            if (ta < tb || (ta == tb && ka < kb))
                best = hit;
        }
    }
    return best;
}

namespace PresenceStore {

namespace {

QSqlDatabase dbFor(const QString &connectionName)
{
    return QSqlDatabase::database(connectionName, false);
}

QString idList(const QList<int> &ids)
{
    QStringList out;
    for (int id : ids)
        out << QString::number(id);
    return out.join(QLatin1Char(','));
}

QString now(const QString &connectionName)
{
    Q_UNUSED(connectionName);
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// Up to 500 ids at a time, for IN (...) lists.
template <typename F>
bool inChunks(const QList<int> &ids, F &&run)
{
    for (qsizetype from = 0; from < ids.size(); from += 500)
        if (!run(ids.mid(from, 500)))
            return false;
    return true;
}

} // namespace

qint64 newestCachedId(const QString &connectionName, const QString &login)
{
    QSqlQuery q(dbFor(connectionName));
    q.prepare(QStringLiteral("SELECT MAX(id) FROM inat_my_observation WHERE login = ?"));
    q.addBindValue(login);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

int cachedCount(const QString &connectionName, const QString &login)
{
    QSqlQuery q(dbFor(connectionName));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM inat_my_observation WHERE login = ?"));
    q.addBindValue(login);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

bool cacheObservations(const QString &connectionName, const QString &login,
                       const QList<taxonomy::Observation> &observations)
{
    QSqlDatabase db = dbFor(connectionName);
    if (!db.transaction())
        return false;
    const QString when = now(connectionName);
    for (const taxonomy::Observation &o : observations) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM inat_my_observation WHERE id = ?"));
        del.addBindValue(o.id);
        del.exec();
        QStringList ancestors;
        for (qint64 a : o.taxonAncestorIds)
            ancestors << QString::number(a);
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO inat_my_observation (id, login, taxon_inat_id, taxon_ancestor_ids, taxon_name, observed_on, "
            "time_observed, latitude, longitude, place_guess, preview_url, fetched_at, time_entered) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(o.id);
        ins.addBindValue(login);
        ins.addBindValue(o.taxonInatId > 0 ? QVariant(o.taxonInatId) : QVariant());
        ins.addBindValue(ancestors.join(QLatin1Char(',')));
        ins.addBindValue(o.taxonCommonName.isEmpty() || o.taxonCommonName == o.taxonName
                             ? o.taxonName
                             : QStringLiteral("%1 · %2").arg(o.taxonCommonName, o.taxonName));
        ins.addBindValue(o.observedOn.left(10));
        ins.addBindValue(o.timeObserved.isValid() ? o.timeObserved.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
                                                  : QString());
        ins.addBindValue(o.latitude ? QVariant(*o.latitude) : QVariant());
        ins.addBindValue(o.longitude ? QVariant(*o.longitude) : QVariant());
        ins.addBindValue(o.placeGuess);
        ins.addBindValue(o.photos.isEmpty() ? QString() : o.photos.first().previewUrl);
        ins.addBindValue(when);
        ins.addBindValue(o.timeEntered.isValid() ? o.timeEntered.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
                                                 : QString());
        if (!ins.exec()) {
            db.rollback();
            return false;
        }
    }
    return db.commit();
}

bool clearCache(const QString &connectionName, const QString &login)
{
    QSqlQuery q(dbFor(connectionName));
    q.prepare(QStringLiteral("DELETE FROM inat_my_observation WHERE login = ?"));
    q.addBindValue(login);
    return q.exec();
}

QList<ExistingObservation> cachedObservations(const QString &connectionName, const QString &login)
{
    QList<ExistingObservation> out;
    QSqlQuery q(dbFor(connectionName));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT id, taxon_inat_id, taxon_ancestor_ids, taxon_name, observed_on, time_observed, "
                             "latitude, longitude, place_guess, preview_url, time_entered FROM inat_my_observation "
                             "WHERE login = ?"));
    q.addBindValue(login);
    if (!q.exec())
        return out;
    while (q.next()) {
        ExistingObservation e;
        e.id = q.value(0).toLongLong();
        e.taxonId = q.value(1).toLongLong();
        for (const QString &a : q.value(2).toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
            e.taxonAncestorIds << a.toLongLong();
        e.taxonName = q.value(3).toString();
        e.date = QDate::fromString(q.value(4).toString(), Qt::ISODate);
        e.time = QDateTime::fromString(q.value(5).toString(), Qt::ISODate);
        if (!e.date.isValid() && e.time.isValid())
            e.date = e.time.date();
        if (!q.value(6).isNull() && !q.value(7).isNull()) {
            e.latitude = q.value(6).toDouble();
            e.longitude = q.value(7).toDouble();
        }
        e.placeGuess = q.value(8).toString();
        if (const QString url = q.value(9).toString(); !url.isEmpty())
            e.photoUrls << url;
        e.timeEntered = QDateTime::fromString(q.value(10).toString(), Qt::ISODate);
        out << e;
    }
    return out;
}

QList<PresencePhoto> photosToCheck(const QString &connectionName)
{
    QList<PresencePhoto> out;
    QSqlQuery q(dbFor(connectionName));
    q.setForwardOnly(true);
    // Each photo's top identification, as the library shows it.
    if (!q.exec(QStringLiteral(
            "SELECT c.id, t.inat_id, t.ancestry, c.captured_on, c.latitude, c.longitude "
            "FROM capture c "
            "JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
            "  ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "JOIN taxon t ON t.id = m.taxon_id "
            "WHERE m.status IN ('auto', 'confirmed') AND c.inat_observation_id IS NULL "
            "  AND NOT EXISTS (SELECT 1 FROM capture_inat_status s WHERE s.capture_id = c.id "
            "                  AND s.status IN ('on_inat', 'not_on_inat')) "
            // Unsorted folders: culled and sorted first.
            "  AND NOT EXISTS (SELECT 1 FROM folder f JOIN unsorted_folder uf ON f.path = uf.path "
            "     OR substr(f.path, 1, length(uf.path) + 1) = uf.path || '/' WHERE f.id = c.folder_id)")))
        return out;
    while (q.next()) {
        PresencePhoto p;
        p.captureId = q.value(0).toInt();
        p.taxonInatId = q.value(1).toLongLong();
        p.taxonAncestorIds = net::inat::ancestryIds(q.value(2).toString());
        const QString when = q.value(3).toString();
        p.day = QDate::fromString(when.left(10), Qt::ISODate);
        if (when.size() > 10)
            p.taken = QDateTime::fromString(when.left(19), Qt::ISODate);
        if (!q.value(4).isNull() && !q.value(5).isNull()) {
            p.latitude = q.value(4).toDouble();
            p.longitude = q.value(5).toDouble();
        }
        out << p;
    }
    return out;
}

bool markOnInat(const QString &connectionName, const QList<int> &captureIds, qint64 observationId,
                PresenceConfidence confidence, const QString &reasons, const QString &decidedBy,
                const QString &checkRun)
{
    if (captureIds.isEmpty())
        return true;
    QSqlDatabase db = dbFor(connectionName);
    const QString when = now(connectionName);
    const QString key = confidence == PresenceConfidence::None ? QStringLiteral("manual") : confidenceKey(confidence);
    if (!inChunks(captureIds, [&](const QList<int> &chunk) {
            QSqlQuery del(db);
            if (!del.exec(QStringLiteral("DELETE FROM capture_inat_status WHERE capture_id IN (%1)").arg(idList(chunk))))
                return false;
            for (int id : chunk) {
                QSqlQuery ins(db);
                ins.prepare(QStringLiteral(
                    "INSERT INTO capture_inat_status (capture_id, status, observation_id, confidence, reasons, "
                    "decided_by, check_run, decided_at) VALUES (?, 'on_inat', ?, ?, ?, ?, ?, ?)"));
                ins.addBindValue(id);
                ins.addBindValue(observationId > 0 ? QVariant(observationId) : QVariant());
                ins.addBindValue(key);
                ins.addBindValue(reasons.isEmpty() ? QVariant() : reasons);
                ins.addBindValue(decidedBy);
                ins.addBindValue(checkRun.isEmpty() ? QVariant() : checkRun);
                ins.addBindValue(when);
                if (!ins.exec())
                    return false;
            }
            return true;
        }))
        return false;
    // Linked as Upload links a photo whose sighting is there: never sent.
    return observationId <= 0 || ObservationUploader::link(connectionName, captureIds, observationId);
}

bool markNotOnInat(const QString &connectionName, const QList<int> &captureIds)
{
    if (!clearStatus(connectionName, captureIds))
        return false;
    QSqlDatabase db = dbFor(connectionName);
    const QString when = now(connectionName);
    for (int id : captureIds) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT INTO capture_inat_status (capture_id, status, confidence, decided_by, "
                                   "decided_at) VALUES (?, 'not_on_inat', 'manual', 'user', ?)"));
        ins.addBindValue(id);
        ins.addBindValue(when);
        if (!ins.exec())
            return false;
    }
    return true;
}

bool markMaybe(const QString &connectionName, int captureId, const PresenceHit &hit, const QString &checkRun)
{
    QSqlDatabase db = dbFor(connectionName);
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM capture_inat_status WHERE capture_id = ? AND status = 'maybe'"));
    del.addBindValue(captureId);
    del.exec();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO capture_inat_status (capture_id, status, observation_id, confidence, reasons, decided_by, "
        "check_run, decided_at) VALUES (?, 'maybe', ?, ?, ?, 'check', ?, ?)"));
    ins.addBindValue(captureId);
    ins.addBindValue(hit.observationId);
    ins.addBindValue(confidenceKey(hit.confidence));
    ins.addBindValue(hit.reasons);
    ins.addBindValue(checkRun);
    ins.addBindValue(now(connectionName));
    return ins.exec();
}

bool clearStatus(const QString &connectionName, const QList<int> &captureIds)
{
    QSqlDatabase db = dbFor(connectionName);
    return inChunks(captureIds, [&](const QList<int> &chunk) {
        const QString ids = idList(chunk);
        // The links these decisions made (a negative inat_photo_id: linked,
        // not uploaded) -- a photo PhotoLife uploaded keeps its observation.
        QSqlQuery unlink(db);
        if (!unlink.exec(QStringLiteral(
                "UPDATE capture SET inat_observation_id = NULL, inat_photo_id = NULL "
                "WHERE id IN (%1) AND inat_photo_id < 0 AND inat_observation_id IN "
                "  (SELECT s.observation_id FROM capture_inat_status s WHERE s.capture_id = capture.id "
                "   AND s.status = 'on_inat')")
                             .arg(ids)))
            return false;
        QSqlQuery del(db);
        return del.exec(QStringLiteral("DELETE FROM capture_inat_status WHERE capture_id IN (%1)").arg(ids));
    });
}

int undoRun(const QString &connectionName, const QString &checkRun)
{
    if (checkRun.isEmpty())
        return 0;
    QList<int> ids;
    QSqlQuery q(dbFor(connectionName));
    q.prepare(QStringLiteral("SELECT capture_id FROM capture_inat_status WHERE check_run = ? AND decided_by = 'check'"));
    q.addBindValue(checkRun);
    if (q.exec())
        while (q.next())
            ids << q.value(0).toInt();
    return clearStatus(connectionName, ids) ? int(ids.size()) : -1;
}

QString lastRun(const QString &connectionName)
{
    QSqlQuery q(dbFor(connectionName));
    return q.exec(QStringLiteral("SELECT MAX(check_run) FROM capture_inat_status WHERE decided_by = 'check'")) && q.next()
               ? q.value(0).toString()
               : QString();
}

QList<Item> items(const QString &connectionName, const QString &status)
{
    QList<Item> out;
    QSqlQuery q(dbFor(connectionName));
    q.setForwardOnly(true);
    const QString where = status == QLatin1String("auto")
                              ? QStringLiteral("s.status = 'on_inat' AND s.decided_by = 'check'")
                              : QStringLiteral("s.status = 'maybe'");
    if (!q.exec(QStringLiteral(
                    "SELECT s.capture_id, s.status, s.confidence, s.decided_by, s.reasons, s.observation_id, "
                    "  COALESCE(NULLIF(c.name_text, ''), c.base_name), "
                    "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                    "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
                    "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                    "  c.captured_on, o.taxon_name, COALESCE(NULLIF(o.time_observed, ''), o.observed_on), "
                    "  o.place_guess, o.preview_url "
                    "FROM capture_inat_status s JOIN capture c ON c.id = s.capture_id "
                    "LEFT JOIN inat_my_observation o ON o.id = s.observation_id "
                    "WHERE %1 "
                    "ORDER BY CASE s.confidence WHEN 'high' THEN 0 WHEN 'medium' THEN 1 ELSE 2 END, "
                    "  s.observation_id, c.captured_on, c.id")
                    .arg(where)))
        return out;
    while (q.next()) {
        Item it;
        it.captureId = q.value(0).toInt();
        it.status = q.value(1).toString();
        it.confidence = confidenceFromKey(q.value(2).toString());
        it.decidedBy = q.value(3).toString();
        it.reasons = q.value(4).toString();
        it.observationId = q.value(5).toLongLong();
        it.photoName = q.value(6).toString();
        it.photoPath = q.value(7).toString();
        it.photoHash = q.value(8).toString();
        it.photoTaken = q.value(9).toString();
        it.observationTaxon = q.value(10).toString();
        it.observationOn = q.value(11).toString();
        it.observationPlace = q.value(12).toString();
        it.observationPreviewUrl = q.value(13).toString();
        out << it;
    }
    return out;
}

} // namespace PresenceStore

} // namespace pl::inat
