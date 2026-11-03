#include "inat/ObservationUploader.h"

#include "inat/PhotoIdentifier.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QtConcurrent>

namespace pl::inat {

namespace {

// A failure the server may have acted on: no answer, or a 5xx.
bool uncertain(int status)
{
    return status == 0 || status >= 500;
}

} // namespace

ObservationUploader::ObservationUploader(net::INatClient &inat, QString connectionName, QObject *parent)
    : QObject(parent), m_inat(inat), m_connectionName(std::move(connectionName))
{
}

QList<UploadPhoto> ObservationUploader::loadPhotos(const QString &connectionName, const QList<int> &captureIds)
{
    QList<UploadPhoto> out;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    for (qsizetype from = 0; from < captureIds.size(); from += 500) {
        QStringList ids;
        for (int id : captureIds.mid(from, 500))
            ids << QString::number(id);
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral(
                "SELECT c.id, c.captured_on, c.latitude, c.longitude, c.inat_observation_id, c.inat_photo_id, "
                "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
                "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                "  t.inat_id, t.name, t.common_name, t.rank, t.ancestry "
                "FROM capture c "
                "LEFT JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
                "  AND status <> 'rejected' AND taxon_id IS NOT NULL "
                "  ORDER BY (status = 'confirmed') DESC, (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
                "LEFT JOIN taxon t ON t.id = m.taxon_id "
                "WHERE c.id IN (%1)")
                        .arg(ids.join(QLatin1Char(',')))))
            continue;
        while (q.next()) {
            UploadPhoto p;
            p.captureId = q.value(0).toInt();
            const QString on = q.value(1).toString();
            p.taken = QDateTime::fromString(on, Qt::ISODate);
            p.hasTime = p.taken.isValid() && on.size() > 10;
            if (!p.taken.isValid() && on.size() >= 10)
                p.taken = QDateTime(QDate::fromString(on.left(10), Qt::ISODate), QTime(12, 0));
            if (!q.value(2).isNull() && !q.value(3).isNull()) {
                p.latitude = q.value(2).toDouble();
                p.longitude = q.value(3).toDouble();
            }
            p.observationId = q.value(4).toLongLong();
            p.photoId = q.value(5).toLongLong();
            if (p.photoId < 0) {   // see link()
                p.linked = p.observationId > 0;
                p.photoId = 0;
            }
            p.path = q.value(6).toString();
            p.contentHash = q.value(7).toString();
            p.label = QFileInfo(p.path).fileName();
            p.taxon.inatId = q.value(8).toLongLong();
            p.taxon.name = q.value(9).toString();
            p.taxon.commonName = q.value(10).toString();
            p.taxon.rank = q.value(11).toString();
            p.taxon.ancestry = q.value(12).toString();
            out << p;
        }

        // An exact copy of a photo that's on iNaturalist (the same file in
        // another folder) is on it too.
        QHash<int, int> at;   // capture id -> its photo in `out`
        for (int i = 0; i < out.size(); ++i)
            if (out.at(i).observationId <= 0)
                at.insert(out.at(i).captureId, i);
        if (at.isEmpty())
            continue;
        QSqlQuery copies(db);
        copies.setForwardOnly(true);
        if (!copies.exec(QStringLiteral(
                "SELECT r.capture_id, c2.inat_observation_id, r2.path "
                "FROM rendition r "
                "JOIN rendition r2 ON r2.content_hash = r.content_hash AND r2.capture_id <> r.capture_id "
                "JOIN capture c2 ON c2.id = r2.capture_id "
                "WHERE r.capture_id IN (%1) AND r.content_hash IS NOT NULL AND r.content_hash <> '' "
                "  AND c2.inat_observation_id IS NOT NULL AND c2.inat_photo_id > 0")
                             .arg(ids.join(QLatin1Char(',')))))
            continue;
        while (copies.next()) {
            const auto it = at.constFind(copies.value(0).toInt());
            if (it == at.constEnd() || out.at(*it).linked)
                continue;
            UploadPhoto &p = out[*it];
            p.observationId = copies.value(1).toLongLong();
            p.linked = true;
            p.copyOf = QFileInfo(copies.value(2).toString()).fileName();
        }
    }
    return out;
}

bool ObservationUploader::link(const QString &connectionName, const QList<int> &captureIds, qint64 observationId)
{
    if (captureIds.isEmpty())
        return true;
    QStringList ids;
    for (int id : captureIds)
        ids << QString::number(id);
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    // A negative inat_photo_id: on iNaturalist by its sighting, not as a
    // photo. Minus its own id, as photo ids are unique (and real ones are
    // never negative).
    q.prepare(QStringLiteral("UPDATE capture SET inat_observation_id = ?, inat_photo_id = -id "
                             "WHERE id IN (%1) AND (inat_photo_id IS NULL OR inat_photo_id < 0)")
                  .arg(ids.join(QLatin1Char(','))));
    q.addBindValue(observationId);
    return q.exec();
}

void ObservationUploader::start(const QList<Job> &jobs)
{
    m_jobs = jobs;
    m_index = -1;
    m_stop = false;
    m_running = true;
    next();
}

void ObservationUploader::next()
{
    ++m_index;
    if (m_stop || m_index >= m_jobs.size()) {
        m_running = false;
        emit finished(false);
        return;
    }
    m_job = m_jobs.at(m_index);
    emit jobStarted(m_job.key);
    ProposedObservation &obs = m_job.observation;
    if (obs.observationId > 0) {
        sendNextPhoto();
        return;
    }
    if (obs.uuid.isEmpty())
        obs.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (obs.uuidSent)
        lookUp({}, 0);   // tried before: it may be there already
    else
        create();
}

void ObservationUploader::create()
{
    const ProposedObservation &obs = m_job.observation;
    net::NewObservation n;
    n.uuid = obs.uuid;
    n.taxonId = obs.taxon.inatId;
    n.speciesGuess = obs.taxon.commonName.isEmpty() ? obs.taxon.name : obs.taxon.commonName;
    const QDateTime at = obs.observedAt();
    n.date = at.date();
    if (obs.hasTime())
        n.time = at.time();
    const QPointF where = obs.where().value_or(QPointF());
    n.latitude = where.y();
    n.longitude = where.x();
    n.geoprivacy = m_job.geoprivacy;
    n.description = obs.note;
    n.fromVision = obs.taxonFromVision;
    m_job.observation.uuidSent = true;

    QPointer<ObservationUploader> self(this);
    m_inat.createObservation(n, [self](net::Outcome<qint64> out) {
        if (!self)
            return;
        if (out.ok())
            self->made(out.value);
        else if (uncertain(out.status))
            self->lookUp(out.error, out.status);
        else
            self->fail(out.error, out.status);
    });
}

// Whether the observation with this uuid exists: then it's carried on with;
// if not, it's made -- unless this follows a failed create, which is
// reported instead (Retry looks again before making it).
void ObservationUploader::lookUp(const QString &failedError, int failedStatus)
{
    QPointer<ObservationUploader> self(this);
    m_inat.findObservationByUuid(m_job.observation.uuid,
                                 [self, failedError, failedStatus](net::Outcome<std::optional<qint64>> out) {
        if (!self)
            return;
        if (out.ok() && out.value) {
            self->made(*out.value);
        } else if (out.ok() && failedError.isEmpty()) {
            self->create();
        } else if (out.ok()) {
            self->fail(failedError, failedStatus);
        } else if (out.status == 401) {
            self->fail(out.error, 401);
        } else {
            self->fail(failedError.isEmpty()
                           ? tr("Couldn't check whether it was made before (%1).").arg(out.error)
                           : tr("%1. It may have been made anyway; Retry checks before sending it again.")
                                 .arg(failedError),
                       failedError.isEmpty() ? out.status : failedStatus);
        }
    });
}

void ObservationUploader::made(qint64 observationId)
{
    ProposedObservation &obs = m_job.observation;
    obs.observationId = observationId;
    QList<int> ids;
    for (UploadPhoto &p : obs.photos) {
        if (p.observationId <= 0)
            ids << p.captureId;
        p.observationId = observationId;
    }
    stamp(ids, observationId);
    emit observationMade(m_job.key, observationId);
    sendNextPhoto();
}

void ObservationUploader::sendNextPhoto()
{
    const ProposedObservation &obs = m_job.observation;
    const int total = int(obs.photos.size());
    int index = -1;
    for (int i = 0; i < total; ++i)
        if (!obs.photos.at(i).done()) {
            index = i;
            break;
        }
    if (index < 0) {
        emit jobFinished(m_job.key, {}, 200);
        next();
        return;
    }
    if (m_stop) {
        emit jobFinished(m_job.key, tr("Stopped, with %1 of %2 photos sent.").arg(total - obs.remaining()).arg(total), 0);
        next();
        return;
    }

    const UploadPhoto photo = obs.photos.at(index);
    if (!QFileInfo::exists(photo.path)) {
        fail(tr("%1 isn't on this computer (it may be kept online only).").arg(photo.label), 0);
        return;
    }
    IdentifyRequest request;
    request.key = QString::number(photo.captureId);
    request.path = photo.path;
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, index, photo, total] {
        const QByteArray jpeg = watcher->result();
        watcher->deleteLater();
        if (jpeg.isEmpty()) {
            fail(tr("%1 couldn't be read.").arg(photo.label), 0);
            return;
        }
        QPointer<ObservationUploader> self(this);
        const QString fileName = QFileInfo(photo.label).completeBaseName() + QStringLiteral(".jpg");
        m_inat.addObservationPhoto(m_job.observation.observationId, jpeg, fileName,
                                   [self, index, photo, total](net::Outcome<qint64> out) {
            if (!self)
                return;
            if (!out.ok()) {
                self->fail(tr("%1: %2").arg(photo.label, out.error), out.status);
                return;
            }
            self->m_job.observation.photos[index].photoId = out.value;
            self->stamp({photo.captureId}, self->m_job.observation.observationId, out.value);
            emit self->photoSent(self->m_job.key, photo.captureId, out.value,
                                 total - self->m_job.observation.remaining(), total);
            self->sendNextPhoto();
        });
    });
    watcher->setFuture(QtConcurrent::run([request] {
        return PhotoIdentifier::renderJpeg(request, kLongestEdge, 90);
    }));
}

void ObservationUploader::fail(const QString &error, int status)
{
    emit jobFinished(m_job.key, error, status);
    if (status == 401) {
        m_running = false;
        emit finished(true);
        return;
    }
    next();
}

bool ObservationUploader::stamp(const QList<int> &captureIds, qint64 observationId, qint64 photoId)
{
    if (captureIds.isEmpty())
        return true;
    QStringList ids;
    for (int id : captureIds)
        ids << QString::number(id);
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    if (photoId > 0) {
        q.prepare(QStringLiteral("UPDATE capture SET inat_observation_id = ?, inat_photo_id = ? WHERE id IN (%1)")
                      .arg(ids.join(QLatin1Char(','))));
        q.addBindValue(observationId);
        q.addBindValue(photoId);
    } else {
        q.prepare(QStringLiteral("UPDATE capture SET inat_observation_id = ? WHERE id IN (%1)")
                      .arg(ids.join(QLatin1Char(','))));
        q.addBindValue(observationId);
    }
    return q.exec();
}

} // namespace pl::inat
