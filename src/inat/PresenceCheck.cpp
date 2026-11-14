#include "inat/PresenceCheck.h"

#include "inat/InatPresence.h"
#include "net/INatClient.h"

#include <QDateTime>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace pl::inat {

PresenceCheck::PresenceCheck(net::INatClient &inat, QString connectionName, QObject *parent)
    : QObject(parent), m_inat(inat), m_connectionName(std::move(connectionName))
{
}

void PresenceCheck::start(const QString &login, bool rereadAll)
{
    if (m_running || login.trimmed().isEmpty())
        return;
    m_login = login.trimmed();
    m_running = true;
    m_cancelled = false;
    m_read = 0;
    if (rereadAll)
        PresenceStore::clearCache(m_connectionName, m_login);
    m_idAbove = PresenceStore::newestCachedId(m_connectionName, m_login);
    fetchNext();
}

void PresenceCheck::cancel()
{
    m_cancelled = true;
}

void PresenceCheck::fetchNext()
{
    if (m_cancelled) {
        Result r;
        r.cancelled = true;
        r.observationsRead = m_read;
        finish(r);
        return;
    }
    emit progress(tr("Reading your observations from iNaturalist…"), m_read, 0);
    QPointer<PresenceCheck> self(this);
    m_inat.fetchAllObservations(
        m_login, 0, 0, m_idAbove,
        [self](net::Outcome<net::ObservationPage> out) {
            if (!self)
                return;
            if (!out.ok()) {
                Result r;
                r.error = out.error;
                r.status = out.status;
                r.observationsRead = self->m_read;
                self->finish(r);
                return;
            }
            const QList<taxonomy::Observation> &page = out.value.results;
            if (page.isEmpty()) {
                // All read: compare.
                emit self->progress(tr("Comparing your photos with your observations…"), 0, 0);
                Result r = checkAgainstCache(self->m_connectionName, self->m_login);
                r.observationsRead = self->m_read;
                self->finish(r);
                return;
            }
            if (!PresenceStore::cacheObservations(self->m_connectionName, self->m_login, page)) {
                Result r;
                r.error = tr("Couldn't keep the observations in the catalogue.");
                self->finish(r);
                return;
            }
            self->m_read += int(page.size());
            for (const taxonomy::Observation &o : page)
                self->m_idAbove = std::max(self->m_idAbove, o.id);
            self->fetchNext();
        },
        /*photosOnly=*/false);
}

PresenceCheck::Result PresenceCheck::checkAgainstCache(const QString &connectionName, const QString &login)
{
    Result r;
    const PresenceMatcher matcher(PresenceStore::cachedObservations(connectionName, login));
    const QList<PresencePhoto> photos = PresenceStore::photosToCheck(connectionName);
    r.photosChecked = int(photos.size());
    r.observationsKnown = PresenceStore::cachedCount(connectionName, login);
    r.run = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.transaction()) {
        r.error = tr("Couldn't write to the catalogue.");
        return r;
    }
    // High: grouped by observation, so each is linked in one step.
    QHash<qint64, QList<int>> high;
    QHash<qint64, QString> highReasons;
    QList<int> stale;   // "maybe" with nothing found now
    for (const PresencePhoto &p : photos) {
        const PresenceHit hit = matcher.best(p);
        switch (hit.confidence) {
        case PresenceConfidence::High:
            high[hit.observationId] << p.captureId;
            highReasons.insert(hit.observationId, hit.reasons);
            break;
        case PresenceConfidence::Medium:
        case PresenceConfidence::Low:
            if (!PresenceStore::markMaybe(connectionName, p.captureId, hit, r.run)) {
                db.rollback();
                r.error = tr("Couldn't write to the catalogue.");
                return r;
            }
            ++(hit.confidence == PresenceConfidence::Medium ? r.medium : r.low);
            break;
        case PresenceConfidence::None:
            stale << p.captureId;
            break;
        }
    }
    for (auto it = high.cbegin(); it != high.cend(); ++it) {
        if (!PresenceStore::markOnInat(connectionName, it.value(), it.key(), PresenceConfidence::High,
                                       highReasons.value(it.key()), QStringLiteral("check"), r.run)) {
            db.rollback();
            r.error = tr("Couldn't write to the catalogue.");
            return r;
        }
        r.marked += int(it.value().size());
    }
    // A "maybe" from an earlier check that no longer matches anything: gone.
    for (qsizetype from = 0; from < stale.size(); from += 500) {
        QStringList ids;
        for (int id : stale.mid(from, 500))
            ids << QString::number(id);
        QSqlQuery(db).exec(QStringLiteral("DELETE FROM capture_inat_status WHERE status = 'maybe' AND capture_id IN (%1)")
                               .arg(ids.join(QLatin1Char(','))));
    }
    if (!db.commit()) {
        r.error = tr("Couldn't write to the catalogue.");
        return r;
    }
    r.ok = true;
    return r;
}

void PresenceCheck::finish(Result result)
{
    m_running = false;
    emit finished(result);
}

} // namespace pl::inat
